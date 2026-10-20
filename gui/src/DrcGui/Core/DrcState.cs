using System.IO;
using System.Text.Json;
using System.Threading;

namespace DrcGui.Core;

public enum RunStatus { Attached, Detached, Bypassed }

/// <summary>音频接入模式（设置页可选；默认虚拟声卡管道）。</summary>
public enum AudioMode
{
    /// <summary>用户态虚拟声卡管道（DrcRouter：虚拟线捕获 → dsp-core → 默认输出；不动原厂驱动链）。</summary>
    VirtualCable,
    /// <summary>APO 注入（DrcApo.dll 经安装器写入设备 FX 链；延迟最低，受槽位互斥限制）。</summary>
    Apo,
}

/// <summary>
/// 参数/状态唯一持有者。字段名照 AGENTS.md §2 冻结词表；
/// 写共享内存 + 异步防抖落 gui/config/drc-config.json；30fps Poll 判定接管状态。
/// </summary>
public sealed class DrcState : IDisposable
{
    public const int EqBands = 10;
    public static readonly double[] EqFreqs = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

    public double CeilingDb = -6;
    public double FloorDb = -40;
    public double MaxBoostDb = 10;
    public double LookaheadMs = 2.5;
    public double ReleaseMs = 120;
    public bool Bypass;
    public double[] EqGainDb = new double[EqBands];
    /// <summary>设置页所选输出设备 GUID（GUI 侧记录；APO 注入由 installer 按设备执行）。</summary>
    public string SelectedDeviceId = "";

    /// <summary>
    /// 所选输出设备**离线**时是否自动改用其它可用真实设备（默认开；耳机断电后自动切到板载输出，
    /// 耳机回来再切回）。关闭 ⇒ 退避等所选设备回来（TASK-012 L2 行为）。
    /// </summary>
    public bool AutoFallbackOutput = true;

    /// <summary>接入模式（默认虚拟声卡管道）。切换时由 GUI 负责 router 进程启停。</summary>
    public AudioMode Mode { get; private set; } = AudioMode.VirtualCable;

    /// <summary>虚拟声卡管道宿主（router 模式下的进程管家）。</summary>
    public readonly RouterHost Router = new();

    public RunStatus Status { get; private set; } = RunStatus.Detached;
    public SharedMemoryChannel.MeterData LastMeter { get; private set; }
    public bool MeterValid { get; private set; }

    public event Action? ParamsChanged;
    public event Action? ModeChanged;
    public event Action? MeterPolled;

    /// <summary>切换接入模式：落盘 + 广播 + 按模式启停 router（APO 模式不动 router 进程）。</summary>
    public void SetMode(AudioMode mode)
    {
        if (Mode == mode) return;
        Mode = mode;
        ScheduleSave();
        if (mode == AudioMode.VirtualCable) Router.Start();
        else Router.Stop();
        ModeChanged?.Invoke();
    }

    /// <summary>启动时按已保存模式就绪（虚拟模式则拉起 router；虚拟线缺席时只报状态不启动）。</summary>
    public void ApplyModeOnStartup()
    {
        if (Mode == AudioMode.VirtualCable) Router.Start();
    }

    readonly SharedMemoryChannel _ipc = new();
    Timer? _saveTimer;
    long _lastAliveMs = long.MinValue / 2;
    long _lastWatchMs = long.MinValue / 2;
    ulong _lastFrames;

    public static string ConfigPath { get; } = ResolveConfigPath();

    /// <summary>最近一次落盘失败的原因（null = 正常）。落盘失败必须能被界面看见，不能静默吞掉。</summary>
    public string? LastSaveError { get; private set; }
    /// <summary>最近一次成功落盘的时刻（本地时间）。</summary>
    public DateTime? LastSavedAt { get; private set; }

    public void LoadConfig()
    {
        try
        {
            MigrateLegacyConfig();
            if (!File.Exists(ConfigPath)) return;
            var dto = JsonSerializer.Deserialize<ConfigDto>(File.ReadAllText(ConfigPath));
            if (dto == null) return;
            CeilingDb = dto.ceilingDb; FloorDb = dto.floorDb; MaxBoostDb = dto.maxBoostDb;
            LookaheadMs = dto.lookaheadMs; ReleaseMs = dto.releaseMs; Bypass = dto.bypass;
            SelectedDeviceId = dto.selectedDeviceId ?? "";
            AutoFallbackOutput = dto.autoFallbackOutput ?? true;
            Mode = string.Equals(dto.mode, "apo", StringComparison.OrdinalIgnoreCase)
                ? AudioMode.Apo : AudioMode.VirtualCable;
            if (dto.eqGainDb != null)
                for (int i = 0; i < EqBands && i < dto.eqGainDb.Length; i++)
                    EqGainDb[i] = dto.eqGainDb[i];
            // 一次性迁移（TASK-014）：老版本 floorDb 下限是 -70、出厂是 -50，这两个区间在
            // 分频上抬结构下"基本抬不到真实脚步"（段包络比全带峰值还低 2–10dB）。
            // 读到 <= -55 的存量值一律抬到 -40，避免用户升级后"还是听不出变化"。
            if (FloorDb <= -55) FloorDb = -40;
            // 读盘值可能带浮点尾巴（旧版本拖滑杆写进去的 -13.556122…）⇒ 按 UI 步进吸附，
            // 保证「显示值 = 滑杆位置 = 下发值」三者一致（2026-10-15 报障③）。
            NormalizeParams(roundEq: true);
        }
        catch { /* 配置损坏时回落默认值 */ }
    }

    /// <summary>
    /// 启动把已加载的配置广播给三页（**只广播**：不写共享内存、不落盘）。
    /// 修 2026-10-15 报障③：启动路径上原本没有任何一次 Apply/事件广播 ⇒ 各页数值文字永远停在 XAML 字面默认值。
    /// </summary>
    public void NotifyParamsChanged() => ParamsChanged?.Invoke();

    /// <summary>钳位到 UI 滑杆范围 + 按步进吸附（0.5dB / 0.5ms / 5ms / 1dB）。</summary>
    public void NormalizeParams(bool roundEq = false)
    {
        CeilingDb = Math.Clamp(RoundTo(CeilingDb, 0.5), -24, -1);
        FloorDb = Math.Clamp(RoundTo(FloorDb, 0.5), -60, -24);
        MaxBoostDb = Math.Clamp(Math.Round(MaxBoostDb), 0, 24);
        LookaheadMs = Math.Clamp(RoundTo(LookaheadMs, 0.5), 1, 10);
        ReleaseMs = Math.Clamp(Math.Round(ReleaseMs / 5) * 5, 20, 500);
        for (int i = 0; i < EqBands; i++)
            EqGainDb[i] = Math.Clamp(roundEq ? RoundTo(EqGainDb[i], 0.5) : EqGainDb[i], -12, 12);
    }

    static double RoundTo(double v, double step) =>
        Math.Round(v / step, MidpointRounding.AwayFromZero) * step;

    public void SetDefaults()
    {
        CeilingDb = -6; FloorDb = -40; MaxBoostDb = 10;
        LookaheadMs = 2.5; ReleaseMs = 120; Bypass = false;
        Array.Clear(EqGainDb);
    }

    /// <summary>写共享内存 + 防抖落盘 + 广播（UI 线程调用）。</summary>
    public void Apply()
    {
        NormalizeParams();
        PushParams();
        ScheduleSave();
        ParamsChanged?.Invoke();
    }

    /// <summary>把当前参数写入共享内存（不落盘、不广播）。</summary>
    public void PushParams()
    {
        _ipc.WriteParams(new SharedMemoryChannel.ParamData(
            (float)CeilingDb, (float)FloorDb, (float)MaxBoostDb,
            (float)LookaheadMs, (float)ReleaseMs,
            EqGainDb.Select(v => (float)v).ToArray(), Bypass));
    }

    /// <summary>30fps 轮询电平区；framesProcessed 推进 = 心跳（3s 无心跳视为未接管）。</summary>
    public void Poll()
    {
        bool wasOpen = _ipc.IsOpen;
        if (!wasOpen) _ipc.TryOpen();            // APO 后启动也能接管上
        if (!wasOpen && _ipc.IsOpen) PushParams();  // 刚接管 → 补推当前参数（含启动加载的配置）
        if (_ipc.IsOpen && _ipc.TryReadMeter(out var m))
        {
            MeterValid = true;
            LastMeter = m;
            // 心跳 = 帧数变化（不是增大）：多流实例并存时新实例帧数会回跳，「只认增大」会漏判
            if (m.Frames != _lastFrames && m.Frames > 0)
                _lastAliveMs = Environment.TickCount64;
            _lastFrames = m.Frames;
        }
        bool attached = _ipc.IsOpen && Environment.TickCount64 - _lastAliveMs < 3000;
        Status = attached ? (Bypass ? RunStatus.Bypassed : RunStatus.Attached) : RunStatus.Detached;

        // 输出设备可用性巡检（TASK-012 L2+）：这里跑在 30fps 的 UI 定时器里，但只需 ~2s 一次
        // （要读注册表枚举端点，别按帧率做）。命中变化时 RouterHost 会重启 router 重定向。
        long now = Environment.TickCount64;
        if (now - _lastWatchMs >= 2000)
        {
            _lastWatchMs = now;
            Router.WatchOutputs();
        }

        MeterPolled?.Invoke();
    }

    /// <summary>请求尽快落盘（设置页改"自动切换输出"这类非参数项时用）。</summary>
    public void SaveConfigSoon() => ScheduleSave();

    void ScheduleSave()
    {
        if (_saveTimer == null)
            _saveTimer = new Timer(_ => SaveNow(), null, 500, Timeout.Infinite);
        else
            _saveTimer.Change(500, Timeout.Infinite);
    }

    void SaveNow()
    {
        try
        {
            var dto = new ConfigDto
            {
                ceilingDb = CeilingDb, floorDb = FloorDb, maxBoostDb = MaxBoostDb,
                lookaheadMs = LookaheadMs, releaseMs = ReleaseMs, bypass = Bypass,
                eqGainDb = EqGainDb.ToArray(), selectedDeviceId = SelectedDeviceId,
                autoFallbackOutput = AutoFallbackOutput,
                mode = Mode == AudioMode.Apo ? "apo" : "virtual",
            };
            var dir = Path.GetDirectoryName(ConfigPath);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.WriteAllText(ConfigPath,
                JsonSerializer.Serialize(dto, new JsonSerializerOptions { WriteIndented = true }));
            LastSavedAt = DateTime.Now;
            LastSaveError = null;
        }
        catch (Exception ex)
        {
            // 落盘失败不留白：装到 Program Files / 目录只读时这里会命中，设置页要如实显示（报障③附带项）
            LastSaveError = ex.Message;
        }
    }

    static string ResolveConfigPath()
    {
        // 用户级数据目录：%LOCALAPPDATA%\DrcGui\drc-config.json
        // 为什么不再放 exe 旁（2026-10-15 用户报障）：装到 Program Files 时那里**不可写**（配置永远存不下），
        // 而且把用户数据和程序文件混在一个目录里，卸载/升级/多人共用一台机器都会互相踩。
        var dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DrcGui");
        return Path.Combine(dir, "drc-config.json");
    }

    /// <summary>老版本把配置写在 exe 旁或仓内 gui/config/ ⇒ 首次运行搬到新位置（只搬一次，原文件保留）。</summary>
    static void MigrateLegacyConfig()
    {
        try
        {
            if (File.Exists(ConfigPath)) return;
            var candidates = new List<string>
            {
                Path.Combine(AppContext.BaseDirectory, "config", "drc-config.json"),
            };
            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            for (int i = 0; i < 8 && dir != null; i++, dir = dir.Parent!)
                if (dir.Name.Equals("gui", StringComparison.OrdinalIgnoreCase))
                    candidates.Add(Path.Combine(dir.FullName, "config", "drc-config.json"));
            foreach (var c in candidates)
            {
                if (!File.Exists(c)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(ConfigPath)!);
                File.Copy(c, ConfigPath, overwrite: false);
                return;
            }
        }
        catch { /* 迁移失败不阻塞启动：走默认值即可 */ }
    }

    public void Dispose()
    {
        SaveNow();
        _saveTimer?.Dispose();
        Router.Dispose();
        _ipc.Dispose();
    }

    sealed class ConfigDto
    {
        public double ceilingDb { get; set; } = -6;
        public double floorDb { get; set; } = -40;
        public double maxBoostDb { get; set; } = 10;
        public double lookaheadMs { get; set; } = 2.5;
        public double releaseMs { get; set; } = 120;
        public bool bypass { get; set; }
        public double[] eqGainDb { get; set; } = new double[EqBands];
        public string? selectedDeviceId { get; set; }
        /// <summary>null（缺字段，老配置）= 默认 true。</summary>
        public bool? autoFallbackOutput { get; set; }
        /// <summary>"virtual"（默认）| "apo"。</summary>
        public string? mode { get; set; }
    }
}
