using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;

namespace DrcGui.Core;

/// <summary>虚拟音频线获取结果（Success = 端点已出现；NeedsRestart = 装完但需重启）。</summary>
public enum CableInstallResult { Success, NeedsRestart, DownloadFailed, SetupNotFound, Canceled, Failed }

/// <summary>
/// 虚拟音频线（VB-CABLE）一键获取：官方站点下载 → 解压 → 提权运行官方安装程序 → 轮询端点。
///
/// 边界（TASK-008）：
/// ① VB-CABLE 为 Donationware，官方未授权再分发/捆绑 ⇒ 本类**不内置驱动包**，只做运行时拉取 +
///    转交官方 setup 执行（避免再分发授权问题）；
/// ② 本类**零注册表写入、零服务操作、零 .sys 复制** —— 驱动落盘与 PnP 安装全部由官方 setup 负责；
/// ③ 单一 URL 有兜底出口（<see cref="HomePageUrl"/>），链接失效时功能可降级而非死锁。
///
/// 静默参数说明：官方页面只写「解压 → 管理员运行 setup → 重启」，**未给出任何命令行参数**。
/// 这里的 <see cref="SetupArgs"/> 来自社区用法，**未经实测**，因此按「尝试」处理：参数无效时官方
/// setup 会照常弹出交互窗口，用户在前台点 Install 即可，流程不因此失败。
/// </summary>
public static class VirtualCableInstaller
{
    /// <summary>官方下载直链（Driver Pack 45，2024-10，Win11/Arm64；链接失效见 <see cref="HomePageUrl"/>）。</summary>
    public const string DownloadUrl = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip";

    /// <summary>官网页面：手动下载兜底出口。</summary>
    public const string HomePageUrl = "https://vb-audio.com/Cable/index.htm";

    /// <summary>静默参数尝试值（社区用法，未实测；无效则官方 setup 弹交互窗口）。</summary>
    const string SetupArgs = "-i -h";

    static string WorkDir => Path.Combine(Path.GetTempPath(), "DrcVirtualCable");

    /// <summary>虚拟音频线是否已就绪（与 RouterHost 同口径：捕获端点含 "CABLE Output"）。</summary>
    public static bool IsReady() => RouterHost.VirtualCablePresent();

    /// <summary>打开官网（手动下载兜底）。</summary>
    public static void OpenHomePage()
    {
        try
        {
            Process.Start(new ProcessStartInfo(HomePageUrl) { UseShellExecute = true });
        }
        catch { /* 无默认浏览器/被策略拦截 ⇒ 忽略，UI 已给出 URL 文本 */ }
    }

    /// <summary>
    /// 执行「下载 → 解压 → 提权安装 → 轮询端点」。<paramref name="progress"/> 接收人类可读进度文本
    /// （调用方在 UI 线程构造 Progress ⇒ 回调自动回 UI 线程）。
    /// </summary>
    public static async Task<CableInstallResult> InstallAsync(
        IProgress<string> progress, CancellationToken ct = default)
    {
        Directory.CreateDirectory(WorkDir);
        string zip = Path.Combine(WorkDir, "VBCABLE_Driver_Pack.zip");

        // 1) 下载（已有非 0 字节缓存则复用，避免重复拉取）
        if (!File.Exists(zip) || new FileInfo(zip).Length == 0)
        {
            try
            {
                progress.Report("正在从官方站点下载驱动包…");
                using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
                http.DefaultRequestHeaders.UserAgent.ParseAdd("DrcGui/1.0");
                using var resp = await http.GetAsync(DownloadUrl, HttpCompletionOption.ResponseHeadersRead, ct);
                resp.EnsureSuccessStatusCode();
                long? total = resp.Content.Headers.ContentLength;
                await using var src = await resp.Content.ReadAsStreamAsync(ct);
                await using var dst = File.Create(zip);
                var buf = new byte[81920];
                long got = 0;
                int n;
                while ((n = await src.ReadAsync(buf, ct)) > 0)
                {
                    await dst.WriteAsync(buf.AsMemory(0, n), ct);
                    got += n;
                    progress.Report(total is > 0
                        ? $"正在下载驱动包… {got * 100 / total.Value}%（{got / 1024} KB / {total.Value / 1024} KB）"
                        : $"正在下载驱动包… {got / 1024} KB");
                }
            }
            catch (OperationCanceledException)
            {
                progress.Report("已取消。");
                return CableInstallResult.Canceled;
            }
            catch (Exception ex)
            {
                progress.Report($"下载失败：{ex.Message}\n可点「官网手动下载」自行获取并安装。");
                return CableInstallResult.DownloadFailed;
            }

            if (!File.Exists(zip) || new FileInfo(zip).Length == 0)
            {
                progress.Report("下载失败：落盘文件为 0 字节。可点「官网手动下载」自行获取。");
                return CableInstallResult.DownloadFailed;
            }
        }

        // 2) 解压
        string pack = Path.Combine(WorkDir, "pack");
        try
        {
            progress.Report("正在解压…");
            if (Directory.Exists(pack)) Directory.Delete(pack, recursive: true);
            Directory.CreateDirectory(pack);
            ZipFile.ExtractToDirectory(zip, pack);
        }
        catch (Exception ex)
        {
            progress.Report($"解压失败：{ex.Message}");
            return CableInstallResult.Failed;
        }

        // 3) 定位官方安装程序
        string? setup =
            Directory.EnumerateFiles(pack, "VBCABLE_Setup_x64.exe", SearchOption.AllDirectories).FirstOrDefault()
            ?? Directory.EnumerateFiles(pack, "VBCABLE_Setup.exe", SearchOption.AllDirectories).FirstOrDefault();
        if (setup == null)
        {
            progress.Report("驱动包内未找到 VBCABLE_Setup*.exe（官方包结构可能已变更），请点「官网手动下载」。");
            return CableInstallResult.SetupNotFound;
        }

        // 4) 提权启动官方安装程序（官方要求管理员模式）
        progress.Report("正在启动官方安装程序…请在窗口中确认安装，装完请重启电脑。");
        try
        {
            var psi = new ProcessStartInfo(setup)
            {
                UseShellExecute = true,
                Verb = "runas",
                Arguments = SetupArgs,
                WorkingDirectory = Path.GetDirectoryName(setup)!,
            };
            using var p = Process.Start(psi);
            if (p == null) return CableInstallResult.Failed;
            await p.WaitForExitAsync(ct);
        }
        catch (System.ComponentModel.Win32Exception)
        {
            progress.Report("已取消管理员授权，安装未执行（可重试或点「官网手动下载」）。");
            return CableInstallResult.Canceled;
        }
        catch (OperationCanceledException)
        {
            progress.Report("已取消。");
            return CableInstallResult.Canceled;
        }
        catch (Exception ex)
        {
            progress.Report($"启动安装程序失败：{ex.Message}");
            return CableInstallResult.Failed;
        }

        // 5) 轮询端点（安装成功通常即时出现；官方仍建议重启）
        progress.Report("正在重新检测虚拟音频线…");
        for (int i = 0; i < 30; i++)
        {
            if (ct.IsCancellationRequested) break;
            if (IsReady()) return CableInstallResult.Success;
            try { await Task.Delay(1000, ct); } catch (OperationCanceledException) { break; }
        }
        return CableInstallResult.NeedsRestart;
    }
}
