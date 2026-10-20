using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace DrcGui.Core;

public enum RouterState { NotRunning, Running, Reconnecting, Failed }

/// <summary>
/// 虚拟声卡管道（用户态）宿主：管理 DrcRouter.exe 生命周期。
/// 链路 = 虚拟音频线捕获端点 → dsp-core → 默认渲染设备；与 APO 模式共用同一块 IPC 共享内存，
/// 因此 GUI 的参数下发 / 电平读取 / 接入状态判定两条路完全一致（见 docs/ARCH-架构总览.md §6）。
/// 本类只做进程启停与状态呈现，不碰音频数据（不违反「禁自创 IPC 通道」——通道仍是共享内存）。
///
/// 端点热插拔（TASK-012）分两层，本类只做 **L2 兜底**：router 自身（L1）在端点失效时不再退出，
/// 而是等待端点回来、按名重解析并重连；所以这里要处理的是它**仍然没能跑起来 / 仍然退了**的两种情形：
///   ① 进程**非用户主动停止**地退出（exit≠0）⇒ 退避（1s→2s→4s，封顶 10s）自动重启；
///   ② 启动**前置条件暂时不满足**（已选的输出设备当前枚举不到 = 耳机断电/未插回；虚拟线尚未就绪）
///      ⇒ 同样退避重试。这一条覆盖"GUI 启动时耳机还没开机、稍后才插上"——此时进程根本没创建，
///      不会有 Exited 事件，只能靠这里等。
/// 用户主动停止（Stop / 切换 APO 模式）后**绝不**自动重启；连续重试超过 MaxRestartAttempts ⇒ Failed。
/// </summary>
public sealed class RouterHost : IDisposable
{
    const string CableNameHint = "CABLE Output";   // VB-CABLE 捕获端点名（router 同口径）
    const int MaxRestartAttempts = 20;             // 连续自动重试上限（超过 ⇒ Failed + 明确文案）
    const int HealthyRunSeconds = 10;              // 进程活过这么久 ⇒ 视为真跑起来过，重试计数归零

    // GUI 崩溃/被强杀时，子进程不能变成"孤儿 router"（实测后果：再启动就两个 router 抢同一条虚拟线 ⇒ 显示未接管）
    static IntPtr _job = IntPtr.Zero;
    const uint JobObjectLimitKillOnJobClose = 0x2000;
    const int JobObjectExtendedLimitInformation = 9;

    Process? _proc;
    readonly List<string> _stderrTail = new();
    readonly object _gate = new();

    bool _userStopped = true;    // 初始从未启动 ⇒ 一律按"用户没让它跑"处理，不自动重启
    bool _disposed;
    Timer? _retryTimer;
    int _attempt;                // 连续自动重试次数（健康运行过 / 用户主动启动后归零）
    DateTime _startedAtUtc;      // 本进程启动时刻（判定"健康运行"用）
    string _activeOutputName = "";       // 当前 router 进程实际用的 `--render` 目标
    bool _activeUsingFallback;           // 该目标是不是"兜底设备"（首选离线被自动切换过来的）
    string _noteOnNextStart = "";        // 重定向时给下一条状态文案带的话

    public RouterState State { get; private set; } = RouterState.NotRunning;
    public string Message { get; private set; } = "未运行";
    public event Action? Changed;

    /// <summary>当前 router 进程实际的输出设备匹配串（未运行 ⇒ 空）。</summary>
    public string ActiveOutputName
    {
        get { lock (_gate) { return _activeOutputName; } }
    }

    /// <summary>当前是否跑在"兜底设备"上（所选设备离线、被自动切换过去）。</summary>
    public bool UsingFallbackOutput
    {
        get { lock (_gate) { return _activeUsingFallback; } }
    }

    /// <summary>当前连续自动重试次数（诊断/测试用；用户主动启动会清零）。</summary>
    public int RestartAttempts
    {
        get { lock (_gate) { return _attempt; } }
    }

    /// <summary>虚拟声卡捕获端点是否就绪（无则 router 必然 exit 3）。</summary>
    public static bool VirtualCablePresent() =>
        WinIntegration.EnumerateCaptureDevices()
            .Any(d => d.Desc.Contains(CableNameHint, StringComparison.OrdinalIgnoreCase));

    /// <summary>虚拟线自身（不能作为输出：会把处理结果送回虚拟线，必然自递归）。</summary>
    static bool IsVirtualLine(string display) =>
        display.Contains("CABLE", StringComparison.OrdinalIgnoreCase) ||
        display.Contains("VB-Audio", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// 「输出设备」下拉选中设备传给 router `--render` 的匹配串。**只认首选**（不含兜底）。
    /// router 的匹配口径 = `PKEY_Device_FriendlyName + " " + PKEY_Device_DeviceDesc`
    /// （见 router/src/wasapi.cpp FindEndpointByName）；其中 FriendlyName 是 Windows 合成的
    /// 「端点描述 (适配器名)」—— 实测即 "扬声器 (<耳机>)" ⇒ 传同构的 DisplayName 精确命中。
    /// 未选 / 选中的是虚拟线自身 / 设备当前枚举不到 ⇒ null。
    /// </summary>
    public static string? OutputDeviceName()
    {
        if (string.IsNullOrEmpty(App.State.SelectedDeviceId)) return null;
        foreach (var d in WinIntegration.EnumerateRenderDevices())
        {
            if (d.Id != App.State.SelectedDeviceId) continue;
            string display = WinIntegration.DisplayName(d.Desc, d.Adapter);
            return IsVirtualLine(display) ? null : display;
        }
        return null;
    }

    /// <summary>用户选中的设备**就是虚拟线自身**（配置错误，不该兜底、也不该空转重试）。</summary>
    public static bool PreferredIsVirtualLine()
    {
        if (string.IsNullOrEmpty(App.State.SelectedDeviceId)) return false;
        foreach (var d in WinIntegration.EnumerateRenderDevices())
            if (d.Id == App.State.SelectedDeviceId)
                return IsVirtualLine(WinIntegration.DisplayName(d.Desc, d.Adapter));
        return false;
    }

    /// <summary>兜底输出 = 枚举顺序里第一个**非虚拟线**的可用渲染端点（本机即板载 <板载声卡> 输出）。</summary>
    public static (string Id, string Name)? FallbackOutput()
    {
        foreach (var d in WinIntegration.EnumerateRenderDevices())
        {
            string display = WinIntegration.DisplayName(d.Desc, d.Adapter);
            if (!IsVirtualLine(display)) return (d.Id, display);
        }
        return null;
    }

    /// <summary>
    /// 本次应当使用的输出：首选可用 ⇒ 首选；首选**离线**且开关开启 ⇒ 兜底设备（Fallback=true）。
    /// 返回 null 的三种"不可自愈"情形：没选设备 / 选了虚拟线自身 / 离线且开关关闭且无兜底。
    /// （端点热插拔 TASK-012：耳机断电后自动改到其它真实设备，回来再切回。）
    /// </summary>
    public static (string Id, string Name, bool Fallback)? ResolveOutput()
    {
        string sel = App.State.SelectedDeviceId;
        if (sel.Length == 0) return null;
        var all = WinIntegration.EnumerateRenderDevices();
        foreach (var d in all)
        {
            if (d.Id != sel) continue;
            string display = WinIntegration.DisplayName(d.Desc, d.Adapter);
            if (IsVirtualLine(display)) return null;        // 选了虚拟线自身 ⇒ 用户配置错误，不兜底
            return (d.Id, display, false);
        }
        if (!App.State.AutoFallbackOutput) return null;      // 离线 + 关了自动切换 ⇒ 交给 L2 退避等它回来
        foreach (var d in all)
        {
            string display = WinIntegration.DisplayName(d.Desc, d.Adapter);
            if (!IsVirtualLine(display)) return (d.Id, display, true);
        }
        return null;
    }

    /// <summary>定位 DrcRouter.exe：① exe 同目录 ② 开发仓 build/router/Release/。</summary>
    public static string? ResolveExe()
    {
        var beside = Path.Combine(AppContext.BaseDirectory, "DrcRouter.exe");
        if (File.Exists(beside)) return beside;
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 8 && dir != null; i++, dir = dir.Parent!)
        {
            var cand = Path.Combine(dir.FullName, "build", "router", "Release", "DrcRouter.exe");
            if (File.Exists(cand)) return cand;
        }
        return null;
    }

    /// <summary>启动（用户点「启动路由器」/ 模式切到虚拟方案 / 启动时按模式拉起）：清零重试计数并立即尝试一次。</summary>
    public void Start()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _userStopped = false;
            _attempt = 0;              // 用户主动启动 ⇒ 重新给满重试额度
        }
        CancelRetry();
        KillOrphanRouters();   // 上一轮 GUI 被强杀/崩溃留下的孤儿先清掉，否则两个 router 会抢同一条虚拟线
        TryStart();
    }

    /// <summary>清掉不是本实例拉起的 DrcRouter（GUI 被强杀后残留的孤儿）。</summary>
    static void KillOrphanRouters()
    {
        try
        {
            foreach (var rp in Process.GetProcessesByName("DrcRouter"))
            {
                try { rp.Kill(entireProcessTree: true); } catch { /* 已退出/无权限 */ }
            }
        }
        catch { /* 枚举失败不影响启动 */ }
    }

    /// <summary>进程级 Job：句柄随 GUI 进程关闭而关闭 ⇒ 内核自动带走 router（防孤儿）。</summary>
    static IntPtr EnsureJob()
    {
        if (_job != IntPtr.Zero) return _job;
        try
        {
            var job = CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero) return IntPtr.Zero;
            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            info.BasicLimitInformation.LimitFlags = JobObjectLimitKillOnJobClose;
            int len = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
            IntPtr buf = Marshal.AllocHGlobal(len);
            try
            {
                Marshal.StructureToPtr(info, buf, false);
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, buf, (uint)len))
                {
                    CloseHandle(job);
                    return IntPtr.Zero;
                }
            }
            finally { Marshal.FreeHGlobal(buf); }
            _job = job;
            return _job;
        }
        catch { return IntPtr.Zero; }
    }

    /// <summary>一次启动尝试。失败时区分"可自愈（退避重试）"与"不可自愈（直接报失败）"。</summary>
    void TryStart()
    {
        lock (_gate)
        {
            if (_disposed || _userStopped) return;
            if (_proc is { HasExited: false }) return;   // 已在运行
        }

        string? exe = ResolveExe();
        if (exe == null)
        {
            // 部署错误（exe 不在）：重试无意义。
            SetState(RouterState.Failed, "未找到 DrcRouter.exe（需先构建 router 或随包分发）");
            return;
        }
        if (!VirtualCablePresent())
        {
            // 虚拟线缺席可能是"正在重装 / 还没装好" ⇒ 可自愈，退避等它回来。
            ScheduleRetry("未检测到虚拟声卡（VB-CABLE 未就绪或正在重装）");
            return;
        }

        // 输出端点必须显式指定真实设备：router 缺省走「系统默认渲染设备」，
        // 而虚拟线场景要求默认设备 = CABLE Input（游戏声才进线）⇒ 缺省会自递归（处理后的音频被送回虚拟线）。
        var outSel = ResolveOutput();
        if (outSel is null)
        {
            if (string.IsNullOrEmpty(App.State.SelectedDeviceId))
            {
                // 用户压根没选设备 ⇒ 不是可自愈故障（空转重试只会刷屏）。
                SetState(RouterState.Failed,
                    "请在「输出设备」选真实播放设备（耳机/扬声器）；虚拟线自身不能作为输出（会自递归）");
            }
            else if (PreferredIsVirtualLine())
            {
                // 选了虚拟线自身 ⇒ 用户配置错误，既不兜底也不重试。
                SetState(RouterState.Failed,
                    "「输出设备」选的是虚拟线自身 ⇒ 会把处理结果送回虚拟线（自递归），请改选真实耳机/扬声器");
            }
            else
            {
                // 已选设备离线且没有可用兜底（或用户关了自动切换）⇒ 退避等它回来。
                ScheduleRetry("所选输出设备当前不可用（耳机未连接/已关机），且未找到可用兜底设备");
            }
            return;
        }
        string outputName = outSel.Value.Name;
        bool usingFallback = outSel.Value.Fallback;

        try
        {
            var psi = new ProcessStartInfo(exe)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            psi.ArgumentList.Add("--render");
            psi.ArgumentList.Add(outputName);
            _stderrTail.Clear();
            var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
            p.ErrorDataReceived += (_, e) => { if (e.Data != null) PushTail(e.Data); };
            p.OutputDataReceived += (_, e) => { if (e.Data != null) PushTail(e.Data); };
            p.Exited += (_, _) => OnExited(p);
            if (!p.Start())
            {
                p.Dispose();
                ScheduleRetry("DrcRouter 启动失败（进程未能创建）");
                return;
            }
            var job = EnsureJob();
            if (job != IntPtr.Zero) AssignProcessToJobObject(job, p.Handle);
            p.BeginErrorReadLine();
            p.BeginOutputReadLine();
            lock (_gate)
            {
                _proc = p;
                _startedAtUtc = DateTime.UtcNow;
                _activeOutputName = outputName;
                _activeUsingFallback = usingFallback;
            }
            string msg = usingFallback
                ? $"运行中（虚拟线 → DRC → {outputName}）｜所选输出设备离线，已自动切换"
                : $"运行中（虚拟线 → DRC → {outputName}）";
            lock (_gate)
            {
                if (_noteOnNextStart.Length > 0)
                {
                    msg += $"（{_noteOnNextStart}）";
                    _noteOnNextStart = "";
                }
            }
            SetState(RouterState.Running, msg);
        }
        catch (Exception ex)
        {
            ScheduleRetry($"启动异常: {ex.Message}");
        }
    }

    /// <summary>
    /// 除"当前实际输出"之外，另一个可用的真实输出（托盘一键切换用）。
    /// 存在的理由：**"无线耳机只关机、USB 接收器还插着"时 Windows 侧零信号**
    /// （端点仍 Active / PnP OK / 麦克风端点不掉 / 接收器照常消费缓冲 —— 实测见 devlog），
    /// 软件无法自动判定 ⇒ 至少给"一次点击"的兜底（不必再进设置页翻下拉框）。
    /// </summary>
    public static (string Id, string Name)? OtherRealOutput()
    {
        string activeName = App.State.Router.ActiveOutputName;
        if (activeName.Length == 0)
        {
            var w = ResolveOutput();
            activeName = w?.Name ?? "";
        }
        foreach (var d in WinIntegration.EnumerateRenderDevices())
        {
            string disp = WinIntegration.DisplayName(d.Desc, d.Adapter);
            if (IsVirtualLine(disp) || disp == activeName) continue;
            return (d.Id, disp);
        }
        return null;
    }

    /// <summary>托盘一键：把输出设备切到"另一个可用真实输出"并落盘（走与设置页同一条重定向路径）。</summary>
    public static bool SwitchOtherOutput()
    {
        var other = OtherRealOutput();
        if (other is null) return false;
        App.State.SelectedDeviceId = other.Value.Id;
        App.State.SaveConfigSoon();
        App.State.Router.OutputSelectionChanged();
        return true;
    }

    /// <summary>
    /// 用户在「输出设备」改了选择：**运行中也要立即生效**。
    /// `--render` 是启动参数、无法热改 ⇒ 运行中改选必须重启 router 进程（约 0.3–0.8s 断音）。
    /// 未运行 ⇒ 直接按新目标启动一次。
    /// </summary>
    public void OutputSelectionChanged()
    {
        bool running;
        lock (_gate) { running = _proc is { HasExited: false }; }
        if (running) Retarget("输出设备已改选");
        else Start();
    }

    /// <summary>
    /// 低频（建议 1–2s 一次）巡检输出设备可用性：首选离线 ⇒ 自动降级到兜底设备；首选回来 ⇒ 切回。
    /// 只在**已在运行**时动手（没跑起来的由 L2 退避重试负责，别两套逻辑抢）。
    /// </summary>
    public void WatchOutputs()
    {
        bool running;
        lock (_gate) { running = _proc is { HasExited: false }; }
        if (!running) return;

        var want = ResolveOutput();
        if (want is null) return;                 // 连兜底都没有：保持现状（等端点回来，L1 自己也在等）
        string wantName = want.Value.Name;
        string activeName;
        lock (_gate) { activeName = _activeOutputName; }
        if (wantName == activeName) return;       // 目标没变 ⇒ 不动

        Retarget(want.Value.Fallback ? "所选输出设备离线，自动切换" : "所选输出设备已恢复，切回");
    }

    /// <summary>把正在运行的 router 重定向到「当前应使用的输出」：停掉旧进程 + 按新目标启动（PID 会变）。</summary>
    public void Retarget(string reason)
    {
        if (_disposed) return;
        bool running;
        lock (_gate) { running = _proc is { HasExited: false }; }
        if (!running)
        {
            Start();
            return;
        }
        lock (_gate) { _noteOnNextStart = reason; }
        Stop();      // 置 _userStopped（防 Exited 竞态触发自动重启）+ 杀进程、清状态
        Start();     // 清 _userStopped、清重试计数，按新目标启动
    }

    /// <summary>用户主动停止：置"用户已停"标志（此后不得自动重启）、取消待定重试、杀进程。</summary>
    public void Stop()
    {
        Process? p;
        lock (_gate)
        {
            _userStopped = true;
            _attempt = 0;
            p = _proc;
            _proc = null;              // 先摘引用：随后 Exited 事件会因"不是我们的进程"而直接返回
            _activeOutputName = "";    // 没进程 ⇒ 没有"当前实际输出"
            _activeUsingFallback = false;
        }
        CancelRetry();
        if (p is { HasExited: false })
        {
            try { p.Kill(entireProcessTree: true); } catch { /* 已退出 */ }
        }
        SetState(RouterState.NotRunning, "已停止");
    }

    void OnExited(Process p)
    {
        int rc;
        try { rc = p.ExitCode; } catch { rc = -1; }

        bool ours;
        bool userStopped;
        double aliveSeconds = 0.0;
        lock (_gate)
        {
            ours = ReferenceEquals(_proc, p);
            userStopped = _userStopped;
            if (ours)
            {
                aliveSeconds = (DateTime.UtcNow - _startedAtUtc).TotalSeconds;
                _proc = null;          // 先摘引用再 Dispose（防 TryStart 撞上已释放对象）
                _activeOutputName = "";
                _activeUsingFallback = false;
                if (aliveSeconds >= HealthyRunSeconds)
                    _attempt = 0;      // 真跑起来过 ⇒ 计数归零（否则"起来就秒退"会把额度耗尽）
            }
        }
        p.Dispose();
        if (!ours) return;             // Stop() 已接管（_proc 已被摘走）⇒ 不重复报状态

        if (rc == 0 || userStopped)
        {
            // rc == 0 = router 正常退出（用户点了停止 / 到点自动停）；用户已停 ⇒ 绝不自动重启。
            SetState(RouterState.NotRunning, rc == 0 ? "已停止（正常退出）" : "已停止");
            return;
        }

        string detail = _stderrTail.Count > 0 ? _stderrTail[^1] : "";
        string hint = rc switch
        {
            3 => "未检测到虚拟声卡（需安装 VB-CABLE 或等价虚拟设备）",
            4 => "音频初始化失败（捕获/渲染端点无法打开）",
            5 => "捕获与渲染格式不一致（采样率/通道不匹配）",
            _ => detail,
        };
        // 非用户停止的非零退出 ⇒ 兜底自动重启（覆盖"启动瞬间端点又消失"等竞态）。
        ScheduleRetry($"路由器异常退出 (exit {rc})：{hint}");
    }

    /// <summary>排程一次退避重试；连续次数超过上限 ⇒ 转 Failed 并说明原因（不再重试）。</summary>
    void ScheduleRetry(string reason)
    {
        int attempt;
        lock (_gate)
        {
            if (_disposed || _userStopped) return;
            attempt = ++_attempt;
        }

        if (attempt > MaxRestartAttempts)
        {
            CancelRetry();
            SetState(RouterState.Failed,
                $"自动重连已放弃（连续 {MaxRestartAttempts} 次未成功）：{reason}");
            return;
        }

        int delayMs = BackoffMs(attempt);
        SetState(RouterState.Reconnecting,
            $"重连中（第 {attempt} 次，{delayMs / 1000.0:0.#} 秒后重试）：{reason}");

        CancelRetry();
        lock (_gate)
        {
            if (_disposed || _userStopped) return;
            _retryTimer = new Timer(_ => TryStart(), null, delayMs, Timeout.Infinite);
        }
    }

    /// <summary>退避序列：1s → 2s → 4s → 封顶 10s（TASK-012 卡面口径）。</summary>
    static int BackoffMs(int attempt) => attempt switch
    {
        <= 1 => 1000,
        2 => 2000,
        3 => 4000,
        _ => 10000,
    };

    void CancelRetry()
    {
        Timer? t;
        lock (_gate)
        {
            t = _retryTimer;
            _retryTimer = null;
        }
        t?.Dispose();
    }

    void PushTail(string line)
    {
        lock (_stderrTail)
        {
            _stderrTail.Add(line.Trim());
            if (_stderrTail.Count > 8) _stderrTail.RemoveAt(0);
        }
    }

    void SetState(RouterState s, string msg)
    {
        State = s;
        Message = msg;
        Changed?.Invoke();
    }

    /// <summary>进程退出/模式切换时调用：置停止标志（禁用自动重启）+ 取消待定重试 + 收尾。</summary>
    public void Dispose()
    {
        lock (_gate) { _disposed = true; }
        Stop();
        CancelRetry();
    }

    // ===== Win32：Job 对象（kill-on-close）=====
    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string? lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr hJob, int infoClass, IntPtr info, uint cbInfoLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);
}
