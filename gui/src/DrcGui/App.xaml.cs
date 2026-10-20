using DrcGui.Core;
using System.Windows;

namespace DrcGui;

public partial class App : Application
{
    public static DrcState State { get; } = new();

    /// <summary>全局快捷键注册结果摘要（设置页显示，见 Core/Hotkeys.cs）。</summary>
    public static string HotkeyStatus { get; set; } = "未注册";

    /// <summary>全局快捷键逐条明细（设置页右栏逐行显示）。</summary>
    public static IReadOnlyList<(string Combo, string Action, bool Ok)> HotkeyRows { get; set; }
        = Array.Empty<(string, string, bool)>();

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        State.LoadConfig();
        State.ApplyModeOnStartup();   // 虚拟方案（默认）⇒ 拉起 DrcRouter；APO 模式不动 router
        var win = new MainWindow();
        win.Show();
        ApplyAutomationArgs(win, e.Args);
    }

    /// <summary>自动化截图辅助（与 MainWindow 的 Ctrl+1/2/3 同源）：
    /// <c>--page N</c> 启动即选页；<c>--tray-menu [x y]</c> 延时打开托盘右键菜单。</summary>
    static void ApplyAutomationArgs(MainWindow win, string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--page" && i + 1 < args.Length && int.TryParse(args[i + 1], out int p))
            {
                win.SelectPage(p);
            }
            else if (args[i] == "--tray-menu")
            {
                double x = 360, y = 300;
                if (i + 2 < args.Length && double.TryParse(args[i + 1], out double ax)
                                        && double.TryParse(args[i + 2], out double ay))
                { x = ax; y = ay; }
                win.Dispatcher.InvokeAsync(async () =>
                {
                    await System.Threading.Tasks.Task.Delay(1200);
                    win.OpenTrayMenuAt(x, y);
                });
            }
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        State.Dispose();
        base.OnExit(e);
    }
}
