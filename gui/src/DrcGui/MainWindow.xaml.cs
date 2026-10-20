using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;
using DrcGui.Core;

namespace DrcGui;

public partial class MainWindow : Window
{
    readonly TrayIcon _tray = new();
    readonly Hotkeys _hotkeys = new();
    readonly DispatcherTimer _meterTimer = new() { Interval = TimeSpan.FromMilliseconds(33) };
    readonly MenuItem _trayBypassItem;
    readonly MenuItem _trayOutputItem;
    readonly ContextMenu _trayMenu;
    bool _exiting;

    [DllImport("user32.dll")]
    static extern bool SetForegroundWindow(IntPtr hWnd);

    public MainWindow()
    {
        InitializeComponent();

        SourceInitialized += (_, _) =>
        {
            var src = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
            _tray.Init(src!);
            _tray.SetBypass(App.State.Bypass);   // 启动即按配置的旁路态显示红/灰图标
            // 全局快捷键（TASK-013 P1）：托盘后台也能改旁路 / 上下限
            _hotkeys.Init(src!);
            _hotkeys.Triggered += OnHotkey;
            _hotkeys.RegisterAll();
            App.HotkeyStatus = _hotkeys.Status;
            App.HotkeyRows = _hotkeys.Entries;
        };
        _tray.OpenRequested += () => { ShowToForeground(); };
        _tray.MenuRequested += ShowTrayMenu;

        _trayMenu = new ContextMenu();
        var openItem = new MenuItem { Header = "打开主界面" };
        openItem.Click += (_, _) => ShowToForeground();
        _trayBypassItem = new MenuItem { Header = "快捷旁路", IsCheckable = true };
        _trayBypassItem.Click += (_, _) =>
        {
            App.State.Bypass = _trayBypassItem.IsChecked;
            App.State.Apply();
        };
        _trayOutputItem = new MenuItem { Header = "切换输出设备" };
        _trayOutputItem.Click += (_, _) => SwitchOutput();
        var exitItem = new MenuItem { Header = "退出" };
        exitItem.Click += (_, _) => ExitApp();
        _trayMenu.Items.Add(openItem);
        _trayMenu.Items.Add(_trayBypassItem);
        _trayMenu.Items.Add(_trayOutputItem);
        _trayMenu.Items.Add(new Separator());
        _trayMenu.Items.Add(exitItem);

        _meterTimer.Tick += (_, _) => App.State.Poll();
        _meterTimer.Start();
        App.State.MeterPolled += OnMeterPolled;
        App.State.ParamsChanged += OnParamsChanged;

        // 启动同步（2026-10-15 报障③）：三页的构造函数只用 _updating 守卫赋初值，文字不会随配置刷新；
        // 而启动路径上原本没有任何 Apply/广播 ⇒ 数值文字永远停在 XAML 字面默认值。
        // 这里在三页均已订阅之后广播一次，让「显示值 = 配置值」。
        App.State.NotifyParamsChanged();

        Closed += (_, _) =>
        {
            _meterTimer.Stop();
            App.State.MeterPolled -= OnMeterPolled;
            App.State.ParamsChanged -= OnParamsChanged;
            _hotkeys.Dispose();
            _tray.Dispose();
        };
    }

    void OnNav(object sender, RoutedEventArgs e)
    {
        if (PageDynamics == null || PageEq == null || PageSettings == null) return;
        PageDynamics.Visibility = NavDynamics.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        PageEq.Visibility = NavEq.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        PageSettings.Visibility = NavSettings.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
    }

    protected override void OnPreviewKeyDown(KeyEventArgs e)
    {
        // Ctrl+1/2/3 页面快捷键（同时服务于自动化截图验证）
        if (Keyboard.Modifiers == ModifierKeys.Control)
        {
            if (e.Key == Key.D1) { NavDynamics.IsChecked = true; e.Handled = true; }
            else if (e.Key == Key.D2) { NavEq.IsChecked = true; e.Handled = true; }
            else if (e.Key == Key.D3) { NavSettings.IsChecked = true; e.Handled = true; }
        }
        base.OnPreviewKeyDown(e);
    }

    // ===== 自动化截图辅助（与 Ctrl+1/2/3 同源；仅影响自身窗口，不触碰其他进程） =====

    /// <summary>按序号选中页面：1=动态范围 / 2=EQ / 3=设置。</summary>
    public void SelectPage(int index)
    {
        if (index == 2) NavEq.IsChecked = true;
        else if (index == 3) NavSettings.IsChecked = true;
        else NavDynamics.IsChecked = true;
    }

    /// <summary>在固定屏幕坐标打开托盘右键菜单（避免依赖光标位置，保证截图确定性）。</summary>
    public void OpenTrayMenuAt(double x, double y)
    {
        _trayBypassItem.IsChecked = App.State.Bypass;
        RefreshTrayOutputItem();
        _trayMenu.PlacementTarget = null;
        _trayMenu.Placement = System.Windows.Controls.Primitives.PlacementMode.AbsolutePoint;
        _trayMenu.HorizontalOffset = x;
        _trayMenu.VerticalOffset = y;
        _trayMenu.IsOpen = true;
    }

    /// <summary>托盘图标是否已注册（自动化核验用）。</summary>
    public bool TrayRegistered => _tray.IsRegistered;

    void OnMinimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    void OnCloseToTray(object sender, RoutedEventArgs e) => Hide();

    void ShowToForeground()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    void ShowTrayMenu()
    {
        SetForegroundWindow(new WindowInteropHelper(this).Handle);
        _trayBypassItem.IsChecked = App.State.Bypass;
        RefreshTrayOutputItem();
        _trayMenu.PlacementTarget = this;
        _trayMenu.Placement = System.Windows.Controls.Primitives.PlacementMode.MousePoint;
        _trayMenu.IsOpen = true;
    }

    /// <summary>
    /// 托盘一键切换输出设备：耳机 ⇄ 扬声器，不用进设置页翻下拉框。
    /// 为什么需要它（TASK-012 实测结论）：**"无线耳机只关机、USB 接收器仍插着"时 Windows 侧零信号**
    /// —— 端点仍 Active、PnP OK/problem 0、麦克风端点不掉、接收器照常消费缓冲（frames 不停），
    /// 所以自动降级在这条路径上物理上不可能；能给的就是"一次点击"。
    /// </summary>
    void SwitchOutput()
    {
        if (RouterHost.SwitchOtherOutput()) return;
        MessageBox.Show("没有可切换的其它真实输出设备（虚拟线自身不能作为输出）。",
            "切换输出设备", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    void RefreshTrayOutputItem()
    {
        var other = RouterHost.OtherRealOutput();
        _trayOutputItem.Header = other is null ? "切换输出设备（无其它可用）" : $"切到 {other.Value.Name}";
        _trayOutputItem.IsEnabled = other is not null;
    }

    /// <summary>全局快捷键 → 改参数（走同一条 Apply 路径，页面/托盘/共享内存自动同步）。</summary>
    void OnHotkey(HotkeyAction action)
    {
        switch (action)
        {
            case HotkeyAction.Bypass:
                App.State.Bypass = !App.State.Bypass;
                break;
            case HotkeyAction.CeilingUp:
                App.State.CeilingDb += 0.5;
                break;
            case HotkeyAction.CeilingDown:
                App.State.CeilingDb -= 0.5;
                break;
            case HotkeyAction.FloorUp:
                App.State.FloorDb += 0.5;
                break;
            case HotkeyAction.FloorDown:
                App.State.FloorDb -= 0.5;
                break;
        }
        App.State.Apply();          // Apply 内含钳位 + 圆整 + 广播 + 防抖落盘
        _tray.SetTooltip($"DRC 动态范围控制 · 上限 {App.State.CeilingDb:F1} / 下限 {App.State.FloorDb:F1} / 抬升 {App.State.MaxBoostDb:F0} dB");
    }

    void OnParamsChanged()
    {
        // 旁路态变化 → 托盘图标红/灰 + 菜单勾选同步
        _trayBypassItem.IsChecked = App.State.Bypass;
        _tray.SetBypass(App.State.Bypass);
    }

    void OnMeterPolled()
    {
        var st = App.State.Status;
        var (brush, text) = st switch
        {
            RunStatus.Attached => (FindResource("GreenBrush"), "已接管"),
            RunStatus.Bypassed => (FindResource("OrangeBrush"), "旁路中"),
            _ => (FindResource("TextDimBrush"), "未接管"),
        };
        StatusDot.Fill = (System.Windows.Media.Brush)brush;
        StatusText.Text = text;
        StatusText.Foreground = (System.Windows.Media.Brush)brush;
    }

    void ExitApp()
    {
        _exiting = true;
        Application.Current.Shutdown();
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        // 关窗 = 托盘化；托盘菜单「退出」才真退
        if (!_exiting)
        {
            e.Cancel = true;
            Hide();
            return;
        }
        base.OnClosing(e);
    }
}
