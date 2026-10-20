using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using DrcGui.Core;

namespace DrcGui.Pages;

public partial class SettingsPage : UserControl
{
    bool _updating;
    bool _installing;              // 虚拟音频线安装流程进行中（TASK-008）
    string? _defaultCableHint;     // 安装面板默认文案（面板重新出现时复位进度文本）

    public SettingsPage()
    {
        InitializeComponent();
        _updating = true;
        SlLookahead.Value = App.State.LookaheadMs;
        SlRelease.Value = App.State.ReleaseMs;
        SlBoost.Value = App.State.MaxBoostDb;
        NumLookahead.Value = App.State.LookaheadMs;
        NumRelease.Value = App.State.ReleaseMs;
        NumBoost.Value = App.State.MaxBoostDb;
        LoadDevices();
        SwAutoStart.IsChecked = WinIntegration.IsAutoStartEnabled();
        SwAutoFallback.IsChecked = App.State.AutoFallbackOutput;
        RbVirtual.IsChecked = App.State.Mode == AudioMode.VirtualCable;
        RbApo.IsChecked = App.State.Mode == AudioMode.Apo;
        _updating = false;
        RefreshRouterUi();
        RefreshConfigUi();

        // 数值框提交（报障④）
        NumLookahead.Committed += v => { App.State.LookaheadMs = v; App.State.Apply(); };
        NumRelease.Committed += v => { App.State.ReleaseMs = v; App.State.Apply(); };
        NumBoost.Committed += v => { App.State.MaxBoostDb = v; App.State.Apply(); };

        App.State.ParamsChanged += OnParamsChanged;
        App.State.ModeChanged += OnModeExternallyChanged;
        App.State.MeterPolled += OnMeterPolled;
        App.State.Router.Changed += OnRouterStateChanged;
        Unloaded += (_, _) =>
        {
            App.State.ParamsChanged -= OnParamsChanged;
            App.State.ModeChanged -= OnModeExternallyChanged;
            App.State.MeterPolled -= OnMeterPolled;
            App.State.Router.Changed -= OnRouterStateChanged;
        };
    }

    // ===== 音频模式（默认虚拟声卡管道）+ 路由器启停 =====
    void OnAudioModeChecked(object sender, RoutedEventArgs e)
    {
        if (_updating || RbVirtual is null || RbApo is null) return;
        App.State.SetMode(RbApo.IsChecked == true ? AudioMode.Apo : AudioMode.VirtualCable);
        RefreshRouterUi();
    }

    void OnModeExternallyChanged()
    {
        _updating = true;
        RbVirtual.IsChecked = App.State.Mode == AudioMode.VirtualCable;
        RbApo.IsChecked = App.State.Mode == AudioMode.Apo;
        _updating = false;
        RefreshRouterUi();
    }

    /// <summary>RouterHost.Changed 可能来自进程退出线程 ⇒ 必须回 UI 线程刷新。</summary>
    void OnRouterStateChanged() => Dispatcher.InvokeAsync(RefreshRouterUi);

    void OnRouterToggle(object sender, RoutedEventArgs e)
    {
        if (App.State.Router.State == RouterState.Running) App.State.Router.Stop();
        else App.State.Router.Start();
        RefreshRouterUi();
    }

    void RefreshRouterUi()
    {
        if (LbRouterState is null) return;
        _defaultCableHint ??= LbCableHint.Text;
        var (text, brush) = App.State.Router.State switch
        {
            RouterState.Running => ("运行中", System.Windows.Media.Brushes.MediumSeaGreen),
            // 端点热插拔（TASK-012）：router 起不来/异常退出后的退避重试等待期（非用户主动停止）
            RouterState.Reconnecting => ("重连中", System.Windows.Media.Brushes.Orange),
            RouterState.Failed => ("启动失败", System.Windows.Media.Brushes.IndianRed),
            _ => ("未运行", System.Windows.Media.Brushes.Gray),
        };
        LbRouterState.Text = text;
        LbRouterState.Foreground = brush;
        BtnRouter.Content = App.State.Router.State switch
        {
            RouterState.Running => "停止路由器",
            RouterState.Reconnecting => "立即重试",   // 点击 = 取消等待、马上再试一次（OnRouterToggle 走 Start()）
            _ => "启动路由器",
        };

        // ===== 虚拟音频线（VB-CABLE）状态行 + 缺线时的安装引导（TASK-008） =====
        bool virtualMode = App.State.Mode == AudioMode.VirtualCable;
        bool cableOk = VirtualCableInstaller.IsReady();
        if (!virtualMode)
        {
            // APO 模式不经过虚拟线 ⇒ 不给「未检测到」的红色假警报
            LbCableState.Text = "不适用（APO 模式）";
            LbCableState.Foreground = System.Windows.Media.Brushes.Gray;
        }
        else
        {
            LbCableState.Text = _installing ? "安装中…" : cableOk ? "已就绪" : "未检测到";
            LbCableState.Foreground = _installing
                ? System.Windows.Media.Brushes.DarkOrange
                : cableOk
                    ? System.Windows.Media.Brushes.MediumSeaGreen
                    : System.Windows.Media.Brushes.IndianRed;
        }

        bool showPanel = virtualMode && !cableOk;
        if (showPanel && !_installing && PnlCableInstall.Visibility != Visibility.Visible)
            LbCableHint.Text = _defaultCableHint!;   // 面板重新出现 ⇒ 复位上次残留的进度文本
        PnlCableInstall.Visibility = showPanel ? Visibility.Visible : Visibility.Collapsed;
        // 缺线时 router 必然 exit 3；APO 模式下 router 不参与 ⇒ 两种情况都不该点
        BtnRouter.IsEnabled = virtualMode && cableOk;

        string hint = App.State.Router.Message;
        if (App.State.Mode == AudioMode.Apo)
            hint = "APO 模式：路由器不参与（DRC 由设备 FX 链内的 DrcApo.dll 处理）";
        else
        {
            hint += "\n① 把 Windows 默认播放设备设为虚拟线的「CABLE Input」（游戏声才进管道）；";
            hint += "\n② 下方「输出设备」卡片选真实耳机/扬声器（播放处理后的声音）。";
            if (!cableOk) hint += "\n未检测到虚拟音频线：可在此一键下载安装。";
            else if (RouterHost.PreferredIsVirtualLine())
                hint += "\n「输出设备」选的是虚拟线自身 ⇒ 会自递归，请改选真实耳机/扬声器。";
            else if (RouterHost.OutputDeviceName() is null)
                hint += App.State.AutoFallbackOutput
                    ? "\n当前「输出设备」已离线：自动改用其它可用输出，插回后自动切回。"
                    : "\n当前「输出设备」已离线：等待它回来（自动切换已关闭）。";
        }
        LbRouterHint.Text = hint;

        // ===== 「输出设备」卡片：当前实际输出 / 是否已自动切换（TASK-012 端点自愈）=====
        var router = App.State.Router;
        if (router.State == RouterState.Running && router.ActiveOutputName.Length > 0)
        {
            LbDeviceAuto.Visibility = Visibility.Visible;
            LbDeviceAuto.Text = router.UsingFallbackOutput
                ? $"当前实际输出：{router.ActiveOutputName}（所选设备离线 ⇒ 已自动切换；插回后自动切回）"
                : $"当前实际输出：{router.ActiveOutputName}";
        }
        else
        {
            LbDeviceAuto.Visibility = Visibility.Collapsed;
        }
    }

    /// <summary>「离线时自动切换」开关：立即落盘；开/关本身也会改变"当前应使用的输出"，故随即重定向。</summary>
    void OnAutoFallbackToggled(object sender, RoutedEventArgs e)
    {
        if (_updating || SwAutoFallback is null) return;
        bool want = SwAutoFallback.IsChecked == true;
        if (App.State.AutoFallbackOutput == want) return;
        App.State.AutoFallbackOutput = want;
        App.State.SaveConfigSoon();
        LoadDevices();                               // 占位项文案随开关变化（"等待回来" ↔ "已自动切换"）
        App.State.Router.OutputSelectionChanged();   // 运行中 ⇒ 立即按新的目标重定向
        RefreshRouterUi();
    }

    // ===== 虚拟音频线一键获取（TASK-008：官方下载 → 提权运行官方 setup → 重新检测） =====
    void OnOpenCableHome(object sender, RoutedEventArgs e) => VirtualCableInstaller.OpenHomePage();

    /// <summary>打开 Windows「声音设置」页：用户需把默认播放设备设为虚拟线的 CABLE Input。</summary>
    void OnOpenSoundSettings(object sender, RoutedEventArgs e)
    {
        try
        {
            Process.Start(new ProcessStartInfo("ms-settings:sound") { UseShellExecute = true });
        }
        catch { /* 无设置应用（Server Core 等）⇒ 忽略，界面已有文字指引 */ }
    }

    async void OnInstallCable(object sender, RoutedEventArgs e)
    {
        if (_installing) return;
        _installing = true;
        BtnCableInstall.IsEnabled = false;
        BtnCableManual.IsEnabled = false;
        RefreshRouterUi();

        // Progress<T> 的 Report 是 IProgress<T> 的显式实现 ⇒ 必须以接口类型持有
        IProgress<string> progress = new Progress<string>(s => LbCableHint.Text = s);
        CableInstallResult result;
        try
        {
            result = await VirtualCableInstaller.InstallAsync(progress);
        }
        catch (Exception ex)   // 兜底：安装流程任何未预期异常都不得让 GUI 崩溃
        {
            progress.Report($"安装异常：{ex.Message}");
            result = CableInstallResult.Failed;
        }

        _installing = false;
        BtnCableInstall.IsEnabled = true;
        BtnCableManual.IsEnabled = true;
        if (result == CableInstallResult.Success && App.State.Mode == AudioMode.VirtualCable)
            App.State.Router.Start();   // 端点已就绪 ⇒ 直接接管，用户无需再点
        RefreshRouterUi();
    }

    void OnLookaheadChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updating || NumLookahead is null) return;
        App.State.LookaheadMs = Math.Round(e.NewValue * 2) / 2;
        NumLookahead.Value = App.State.LookaheadMs;
        App.State.Apply();
    }

    void OnReleaseChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updating || NumRelease is null) return;
        App.State.ReleaseMs = Math.Round(e.NewValue / 5) * 5;
        NumRelease.Value = App.State.ReleaseMs;
        App.State.Apply();
    }

    void OnBoostChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updating || NumBoost is null) return;
        App.State.MaxBoostDb = Math.Round(e.NewValue);
        NumBoost.Value = App.State.MaxBoostDb;
        App.State.Apply();
    }

    void OnResetDefaults(object sender, RoutedEventArgs e)
    {
        App.State.SetDefaults();
        App.State.Apply();
    }

    // ===== 输出设备（只读枚举；选择结果记录进 config，供后续安装器/APO 消费） =====
    void LoadDevices()
    {
        CbDevice.Items.Clear();
        var devices = WinIntegration.EnumerateRenderDevices();
        if (devices.Count == 0)
        {
            CbDevice.Items.Add(new ComboBoxItem { Content = "（未检测到活动输出设备）", IsEnabled = false });
            CbDevice.SelectedIndex = 0;
            return;
        }
        foreach (var d in devices)
            CbDevice.Items.Add(new ComboBoxItem
            {
                // 完整名 = 端点描述 (适配器名)，与 Windows 声音设置一致；重名端点靠它区分
                Content = WinIntegration.DisplayName(d.Desc, d.Adapter),
                Tag = d.Id,
            });
        int idx = devices.FindIndex(d => d.Id == App.State.SelectedDeviceId);
        if (idx < 0 && App.State.SelectedDeviceId.Length > 0)
        {
            // 端点热插拔（TASK-012）：已保存的输出设备当前不在（耳机关机/未插回）⇒ **不要**静默
            // 改成别的设备（那会让 RouterHost 的退避重试永远等不到人），保留用户选择并插占位项；
            // 设备回来后由 RouterHost 的退避重试自动接上。
            _updating = true;
            CbDevice.Items.Insert(0, new ComboBoxItem
            {
                Content = App.State.AutoFallbackOutput
                    ? "（已选设备离线 ⇒ 已自动改用其它输出，插回后切回）"
                    : "（已选设备当前不可用，等待它回来…）",
                Tag = App.State.SelectedDeviceId,
            });
            CbDevice.SelectedIndex = 0;
            _updating = false;
        }
        else
        {
            CbDevice.SelectedIndex = idx >= 0 ? idx : 0;
            App.State.SelectedDeviceId = devices[CbDevice.SelectedIndex].Id;
        }
    }

    void OnDeviceChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || CbDevice is null) return;
        if (CbDevice.SelectedItem is ComboBoxItem { Tag: string id })
        {
            string old = App.State.SelectedDeviceId;
            App.State.SelectedDeviceId = id;
            App.State.SaveConfigSoon();
            // 用户换了输出设备 ⇒ **立即生效**（`--render` 是启动参数，运行中改选必须重启 router 重定向；
            // 未运行则直接按新目标启动）。修复"改了设备不重新开关就永远没声音"。
            if (old != id && App.State.Mode == AudioMode.VirtualCable)
                App.State.Router.OutputSelectionChanged();
            RefreshRouterUi();
        }
    }

    // ===== 开机自启（HKCU Run 值） =====
    void OnAutoStartToggled(object sender, RoutedEventArgs e)
    {
        if (_updating || SwAutoStart is null) return;
        bool want = SwAutoStart.IsChecked == true;
        if (!WinIntegration.SetAutoStart(want))
        {
            _updating = true;
            SwAutoStart.IsChecked = WinIntegration.IsAutoStartEnabled();
            _updating = false;
            MessageBox.Show("写入开机自启失败（HKCU 启动项不可写）。", "DRC 动态范围控制",
                MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    void OnParamsChanged()
    {
        _updating = true;
        SlLookahead.Value = App.State.LookaheadMs;
        SlRelease.Value = App.State.ReleaseMs;
        SlBoost.Value = App.State.MaxBoostDb;
        NumLookahead.Value = App.State.LookaheadMs;
        NumRelease.Value = App.State.ReleaseMs;
        NumBoost.Value = App.State.MaxBoostDb;
        _updating = false;
    }

    /// <summary>配置落盘状态（路径 / 最近保存时刻 / 失败原因）。失败必须可见。</summary>
    // 注意：缓存键必须包含快捷键状态 —— 本页构造时（SourceInitialized 之前）App.HotkeyStatus 还是
    // "未注册"，若只按"落盘时刻/错误"判断是否需要刷新，就会把"未注册"永久钉在界面上（实测踩到）。
    (DateTime?, string?, string) _cfgShown;
    /// <summary>右栏「全局快捷键」逐条渲染（只建一次，数据来自 App.HotkeyRows）。</summary>
    bool _hotkeysRendered;
    void RefreshHotkeyRows()
    {
        if (HotkeyRows is null || _hotkeysRendered) return;
        if (App.HotkeyRows.Count == 0) return;      // 尚未注册完成 ⇒ 等下一次轮询
        _hotkeysRendered = true;
        var dim = (System.Windows.Media.Brush)FindResource("TextDimBrush");
        var okBrush = System.Windows.Media.Brushes.MediumSeaGreen;
        var badBrush = System.Windows.Media.Brushes.IndianRed;
        int okCount = 0;
        foreach (var (combo, action, ok) in App.HotkeyRows)
        {
            if (ok) okCount++;
            var row = new Grid { Margin = new Thickness(0, 4, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(104) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var c = new TextBlock
            {
                Text = combo, FontSize = 12, Foreground = (System.Windows.Media.Brush)FindResource("TextSubBrush"),
                VerticalAlignment = VerticalAlignment.Center,
            };
            var a = new TextBlock
            {
                Text = action, FontSize = 12, Foreground = ok ? dim : badBrush,
                VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 8, 0),
            };
            Grid.SetColumn(a, 1);
            var s = new TextBlock
            {
                Text = ok ? "已注册" : "被占用", FontSize = 11,
                Foreground = ok ? okBrush : badBrush,
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(s, 2);
            row.Children.Add(c); row.Children.Add(a); row.Children.Add(s);
            HotkeyRows.Children.Add(row);
        }
        LbHotkeyCount.Text = $"{okCount}/{App.HotkeyRows.Count}";
        LbHotkeyCount.Foreground = okCount == App.HotkeyRows.Count ? okBrush : badBrush;
    }

    void RefreshConfigUi()
    {
        if (LbConfig is null) return;
        var key = (App.State.LastSavedAt, App.State.LastSaveError, App.HotkeyStatus);
        if (key == _cfgShown && LbConfig.Text.Length > 2) return;
        _cfgShown = key;

        RefreshHotkeyRows();
        string text = $"配置：{DrcState.ConfigPath}";
        if (!string.IsNullOrEmpty(App.State.LastSaveError))
        {
            text += $"\n保存失败：{App.State.LastSaveError}";
            LbConfig.Foreground = System.Windows.Media.Brushes.IndianRed;
        }
        else
        {
            text += App.State.LastSavedAt is { } t ? $"\n已保存 {t:HH:mm:ss}" : "\n（尚未保存过）";
            LbConfig.Foreground = (System.Windows.Media.Brush)FindResource("TextDimBrush");
        }
        LbConfig.Text = text;
    }

    void OnMeterPolled()
    {
        RefreshConfigUi();          // 防抖落盘是 500ms 异步的 ⇒ 保存结果要在轮询里补一次显示
        var st = App.State.Status;
        var brush = st switch
        {
            RunStatus.Attached => System.Windows.Media.Brushes.MediumSeaGreen,
            RunStatus.Bypassed => System.Windows.Media.Brushes.DarkOrange,
            _ => System.Windows.Media.Brushes.Gray,
        };
        string text = st switch
        {
            RunStatus.Attached => "已接管",
            RunStatus.Bypassed => "旁路中",
            _ => "未接管",
        };
        BigDot.Fill = brush;
        BigStatus.Text = text;
        BigStatus.Foreground = brush;

        if (st == RunStatus.Detached)
        {
            StatusHint.Text = App.State.Mode == AudioMode.VirtualCable
                ? "共享内存不存在或心跳停止：路由器未在运行 / 虚拟线未就绪"
                : "共享内存不存在或心跳停止：未检测到 APO 接管";
            LbRate.Text = LbLatency.Text = LbGain.Text = LbFrames.Text = "—";
            return;
        }
        StatusHint.Text = App.State.Mode == AudioMode.VirtualCable
            ? "已连接路由器 · 电平区心跳正常（30fps 轮询）"
            : "已连接 APO · 电平区心跳正常（30fps 轮询）";
        var m = App.State.LastMeter;
        LbRate.Text = m.SampleRate > 0 ? $"{m.SampleRate} Hz / {m.Channels}ch" : "—";
        LbLatency.Text = m.LatencyMs > 0 ? $"{m.LatencyMs:F1} ms" : "—";
        LbGain.Text = m.GainReductionDb > 0 ? $"-{m.GainReductionDb:F1} dB" : "0.0 dB";
        LbFrames.Text = m.Frames > 0 ? m.Frames.ToString("N0") : "—";
    }
}
