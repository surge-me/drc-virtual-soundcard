using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;
using DrcGui.Core;

namespace DrcGui.Pages;

public partial class DynamicsPage : UserControl
{
    bool _updating;
    double _peakInDb = -100, _peakOutDb = -100;

    // ===== 实时电平滚动包络（10s × 30fps 环形缓冲） =====
    const int HistN = 300;                       // 300 帧 ≈ 10 秒
    readonly double[] _histIn = new double[HistN];
    readonly double[] _histOut = new double[HistN];
    int _histCount;                              // 已填充样本数
    int _histHead;                               // 下一写入位（环形）

    public DynamicsPage()
    {
        InitializeComponent();
        Array.Fill(_histIn, -80);
        Array.Fill(_histOut, -80);
        _updating = true;
        SlCeiling.Value = App.State.CeilingDb;
        SlFloor.Value = App.State.FloorDb;
        SlBoost.Value = App.State.MaxBoostDb;
        NumCeiling.Value = App.State.CeilingDb;
        NumFloor.Value = App.State.FloorDb;
        NumBoost.Value = App.State.MaxBoostDb;
        SwBypass.IsChecked = App.State.Bypass;
        _updating = false;

        // 数值框提交（报障④）：Enter/失焦/步进/右键默认值 ⇒ 直接回落参数并下发
        NumCeiling.Committed += v => { App.State.CeilingDb = v; App.State.Apply(); };
        NumFloor.Committed += v => { App.State.FloorDb = v; App.State.Apply(); };
        NumBoost.Committed += v => { App.State.MaxBoostDb = v; App.State.Apply(); };

        Loaded += (_, _) => { RedrawCurve(); RedrawLive(); };
        App.State.ParamsChanged += OnParamsChanged;
        App.State.MeterPolled += OnMeterPolled;
        Unloaded += (_, _) =>
        {
            App.State.ParamsChanged -= OnParamsChanged;
            App.State.MeterPolled -= OnMeterPolled;
        };
    }

    void OnCeilingChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updating || NumCeiling is null) return;
        App.State.CeilingDb = e.NewValue;
        NumCeiling.Value = App.State.CeilingDb;
        App.State.Apply();
    }

    void OnFloorChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updating || NumFloor is null) return;
        App.State.FloorDb = e.NewValue;
        NumFloor.Value = App.State.FloorDb;
        App.State.Apply();
    }

    void OnBoostChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updating || NumBoost is null) return;
        App.State.MaxBoostDb = Math.Round(e.NewValue);
        NumBoost.Value = App.State.MaxBoostDb;
        App.State.Apply();
    }

    void OnBypassToggled(object sender, RoutedEventArgs e)
    {
        if (_updating || SwBypass is null) return;
        App.State.Bypass = SwBypass.IsChecked == true;
        App.State.Apply();
    }

    void OnParamsChanged()
    {
        // 其他页面（设置页恢复默认 / 托盘快捷旁路 / 全局快捷键）改参数时同步本页 UI
        _updating = true;
        SlCeiling.Value = App.State.CeilingDb;
        SlFloor.Value = App.State.FloorDb;
        SlBoost.Value = App.State.MaxBoostDb;
        NumCeiling.Value = App.State.CeilingDb;
        NumFloor.Value = App.State.FloorDb;
        NumBoost.Value = App.State.MaxBoostDb;
        SwBypass.IsChecked = App.State.Bypass;
        _updating = false;
        RedrawCurve();
        RedrawLive();
    }

    // ===== 电平表（30fps 读 meter 区 + 峰值保持） =====
    void OnMeterPolled()
    {
        if (!App.State.MeterValid)
        {
            InFill.Width = OutFill.Width = 0;
            Canvas.SetLeft(InPeak, 0); Canvas.SetLeft(OutPeak, 0);
            InVal.Text = OutVal.Text = BoostVal.Text = "—";
            BoostFill.Width = 0;
            AppendHistory(-80, -80);
            RedrawLive();
            return;
        }
        var m = App.State.LastMeter;
        double inDb = m.InPeakDb, outDb = m.OutPeakDb;
        AppendHistory(inDb, outDb);
        // 峰值保持：保持值按 0.35dB/帧 衰减
        _peakInDb = Math.Max(_peakInDb - 0.35, inDb);
        _peakOutDb = Math.Max(_peakOutDb - 0.35, outDb);

        double w = DbToNorm(inDb) * TrackWidth(ActualWidth), wp = DbToNorm(_peakInDb) * TrackWidth(ActualWidth);
        InFill.Width = w;
        InPeak.Margin = new Thickness(wp, 0, 0, 0);
        double w2 = DbToNorm(outDb) * TrackWidth(ActualWidth), wp2 = DbToNorm(_peakOutDb) * TrackWidth(ActualWidth);
        OutFill.Width = w2;
        OutPeak.Margin = new Thickness(wp2, 0, 0, 0);
        InVal.Text = inDb <= -79.9 ? "—" : $"{inDb:F1} dB";
        OutVal.Text = outDb <= -79.9 ? "—" : $"{outDb:F1} dB";

        // 抬升量 = 输出 − 输入（用现有 meter 字段算，不改 IPC）
        double lift = outDb - inDb;
        BoostVal.Text = (inDb <= -79.9 || outDb <= -79.9) ? "—"
                        : lift >= 0.05 ? $"+{lift:F1} dB" : "0.0 dB";
        BoostFill.Width = Math.Clamp(lift / 24.0, 0, 1) * TrackWidth(ActualWidth);
        RedrawLive();
    }

    // ===== 实时电平：滚动包络 + 压限/抬升分色 =====
    void AppendHistory(double inDb, double outDb)
    {
        _histIn[_histHead] = Math.Clamp(inDb, -80, 0);
        _histOut[_histHead] = Math.Clamp(outDb, -80, 0);
        _histHead = (_histHead + 1) % HistN;
        if (_histCount < HistN) _histCount++;
    }

    void OnLiveSizeChanged(object sender, SizeChangedEventArgs e) => RedrawLive();

    /// <summary>取时序第 i 帧（i=0 最旧）：环形缓冲展开。</summary>
    (double In, double Out) HistAt(int i)
    {
        int idx = (_histHead - _histCount + i + HistN * 2) % HistN;
        return (_histIn[idx], _histOut[idx]);
    }

    void RedrawLive()
    {
        var c = LiveCanvas;
        if (c == null || c.ActualWidth < 40 || c.ActualHeight < 40) return;
        c.Children.Clear();
        double W = c.ActualWidth, H = c.ActualHeight;
        const double padL = 40, padR = 12, padT = 8, padB = 22;

        double Y(double db) => padT + (1.0 - (db + 80.0) / 80.0) * (H - padT - padB);
        double X(int i) => padL + (_histCount <= 1 ? 0 : (double)i / (HistN - 1)) * (W - padL - padR);

        var grid = Color.FromRgb(0x33, 0x33, 0x3A);
        var dim = Color.FromRgb(0x6A, 0x6A, 0x72);

        // 网格 + 纵轴刻度（每 10dB）
        for (int db = -80; db <= 0; db += 10)
        {
            c.Children.Add(new Line
            {
                X1 = padL, Y1 = Y(db), X2 = W - padR, Y2 = Y(db),
                Stroke = new SolidColorBrush(grid), StrokeThickness = 1,
            });
            var ty = new TextBlock { Text = $"{db}", FontSize = 10, Foreground = new SolidColorBrush(dim) };
            Canvas.SetLeft(ty, 6); Canvas.SetTop(ty, Y(db) - 7);
            c.Children.Add(ty);
        }
        var axisR = new TextBlock { Text = "现在 →", FontSize = 10, Foreground = new SolidColorBrush(dim) };
        Canvas.SetLeft(axisR, W - padR - 40); Canvas.SetTop(axisR, H - padB + 4);
        c.Children.Add(axisR);
        var axisL = new TextBlock { Text = "← 10s 前", FontSize = 10, Foreground = new SolidColorBrush(dim) };
        Canvas.SetLeft(axisL, padL); Canvas.SetTop(axisL, H - padB + 4);
        c.Children.Add(axisL);

        double ceiling = App.State.CeilingDb, floor = App.State.FloorDb;

        // ceiling / floor 参考线（与右列滑杆联动）
        c.Children.Add(new Line
        {
            X1 = padL, Y1 = Y(ceiling), X2 = W - padR, Y2 = Y(ceiling),
            Stroke = new SolidColorBrush(Color.FromRgb(0xE8, 0x3C, 0x3C)) { Opacity = 0.55 },
            StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 3, 3 },
        });
        c.Children.Add(new Line
        {
            X1 = padL, Y1 = Y(floor), X2 = W - padR, Y2 = Y(floor),
            Stroke = new SolidColorBrush(Color.FromRgb(0x3D, 0xBE, 0x7B)) { Opacity = 0.55 },
            StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 3, 3 },
        });
        var tc = new TextBlock { Text = $"ceiling {ceiling:F0}", FontSize = 10, Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0x3C, 0x3C)) { Opacity = 0.8 } };
        Canvas.SetLeft(tc, W - padR - 78); Canvas.SetTop(tc, Y(ceiling) - 15);
        c.Children.Add(tc);
        var tf = new TextBlock { Text = $"floor {floor:F0}", FontSize = 10, Foreground = new SolidColorBrush(Color.FromRgb(0x3D, 0xBE, 0x7B)) { Opacity = 0.8 } };
        Canvas.SetLeft(tf, W - padR - 60); Canvas.SetTop(tf, Y(floor) + 3);
        c.Children.Add(tf);

        if (_histCount < 2) return;

        // 处理痕迹：|in-out| > 0.2dB 的区段按方向分色（压限红 / 抬升绿）
        c.Children.Add(MakeProcessPolys());

        // 输入 / 输出包络线
        var pin = new PointCollection(_histCount);
        var pout = new PointCollection(_histCount);
        for (int i = 0; i < _histCount; i++)
        {
            var (a, b) = HistAt(i);
            pin.Add(new Point(X(i), Y(a)));
            pout.Add(new Point(X(i), Y(b)));
        }
        c.Children.Add(new Polyline { Points = pin, Stroke = new SolidColorBrush(Color.FromRgb(0x9A, 0xA0, 0xA6)), StrokeThickness = 1.2, Opacity = 0.75 });
        c.Children.Add(new Polyline { Points = pout, Stroke = new SolidColorBrush(Color.FromRgb(0xE8, 0x3C, 0x3C)), StrokeThickness = 1.6 });
    }

    /// <summary>把 |in-out|>0.2dB 的区段画成 4 点多边形（压限红/抬升绿，半透明）。</summary>
    UIElement MakeProcessPolys()
    {
        var panel = new Canvas();
        const double thresh = 0.1;
        int i = 0;
        while (i < _histCount)
        {
            var (a, b) = HistAt(i);
            double d = b - a;
            bool lim = d < -thresh, boo = d > thresh;
            if (!lim && !boo) { i++; continue; }
            int j = i;
            while (j < _histCount)
            {
                var (a2, b2) = HistAt(j);
                double d2 = b2 - a2;
                if ((d2 < -thresh) != lim || (d2 > thresh) != boo) break;
                j++;
            }
            double YOf(double db) => 8.0 + (1.0 - (db + 80.0) / 80.0) * (LiveCanvas.ActualHeight - 8.0 - 22.0);
            double XL(int idx) => 40.0 + (_histCount <= 1 ? 0 : (double)idx / (HistN - 1)) * (LiveCanvas.ActualWidth - 40.0 - 12.0);
            var poly = new Polygon
            {
                Points = new PointCollection
                {
                    new Point(XL(i), YOf(Math.Max(HistAt(i).In, HistAt(i).Out))),
                    new Point(XL(j - 1), YOf(Math.Max(HistAt(j - 1).In, HistAt(j - 1).Out))),
                    new Point(XL(j - 1), YOf(Math.Min(HistAt(j - 1).In, HistAt(j - 1).Out))),
                    new Point(XL(i), YOf(Math.Min(HistAt(i).In, HistAt(i).Out))),
                },
                Fill = new SolidColorBrush(lim
                    ? Color.FromRgb(0xE8, 0x3C, 0x3C)
                    : Color.FromRgb(0x3D, 0xBE, 0x7B)) { Opacity = 0.30 },
            };
            panel.Children.Add(poly);
            i = j;
        }
        return panel;
    }

    double TrackWidth(double total)
    {
        // 条区宽度 ≈ 总宽 - label 44 - 数值 70（近似；条本身 ClipToBounds 保护）
        return Math.Max(0, total - 114);
    }

    static double DbToNorm(double db) => Math.Clamp((db + 80.0) / 80.0, 0, 1);

    // ===== 处理曲线（x=输入 dB，y=输出 dB） =====
    void OnCurveSizeChanged(object sender, SizeChangedEventArgs e) => RedrawCurve();

    void RedrawCurve()
    {
        var c = CurveCanvas;
        if (c == null || c.ActualWidth < 40 || c.ActualHeight < 40) return;
        c.Children.Clear();
        double W = c.ActualWidth, H = c.ActualHeight;
        const double padL = 40, padB = 24, padT = 8, padR = 12;

        double X(double db) => padL + (db + 80.0) / 80.0 * (W - padL - padR);
        double Y(double db) => padT + (1.0 - (db + 80.0) / 80.0) * (H - padT - padB);

        // 网格（每 10dB）
        for (int db = -80; db <= 0; db += 10)
        {
            var gx = new Line
            {
                X1 = X(db), Y1 = padT, X2 = X(db), Y2 = H - padB,
                Stroke = new SolidColorBrush(Color.FromRgb(0x33, 0x33, 0x3A)), StrokeThickness = 1,
            };
            var gy = new Line
            {
                X1 = padL, Y1 = Y(db), X2 = W - padR, Y2 = Y(db),
                Stroke = new SolidColorBrush(Color.FromRgb(0x33, 0x33, 0x3A)), StrokeThickness = 1,
            };
            c.Children.Add(gx);
            c.Children.Add(gy);
            var tx = new TextBlock { Text = $"{db}", FontSize = 10, Foreground = new SolidColorBrush(Color.FromRgb(0x6A, 0x6A, 0x72)) };
            Canvas.SetLeft(tx, X(db) - 10); Canvas.SetTop(tx, H - padB + 4);
            c.Children.Add(tx);
            var ty = new TextBlock { Text = $"{db}", FontSize = 10, Foreground = new SolidColorBrush(Color.FromRgb(0x6A, 0x6A, 0x72)) };
            Canvas.SetLeft(ty, 6); Canvas.SetTop(ty, Y(db) - 7);
            c.Children.Add(ty);
        }

        // 单位标注
        var axisX = new TextBlock { Text = "输入 dB", FontSize = 10, Foreground = new SolidColorBrush(Color.FromRgb(0x6A, 0x6A, 0x72)) };
        Canvas.SetLeft(axisX, W - padR - 44); Canvas.SetTop(axisX, H - padB + 4);
        c.Children.Add(axisX);

        // 对角虚线（单位增益）
        c.Children.Add(new Line
        {
            X1 = X(-80), Y1 = Y(-80), X2 = X(0), Y2 = Y(0),
            Stroke = new SolidColorBrush(Color.FromRgb(0x6A, 0x6A, 0x72)),
            StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 4, 4 },
        });

        // ceiling / floor 参考线
        var ceiling = App.State.CeilingDb;
        var floor = App.State.FloorDb;
        var maxBoost = App.State.MaxBoostDb;
        var cl = new Line
        {
            X1 = padL, Y1 = Y(ceiling), X2 = W - padR, Y2 = Y(ceiling),
            Stroke = new SolidColorBrush(Color.FromRgb(0xE8, 0x3C, 0x3C)) { Opacity = 0.55 },
            StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 3, 3 },
        };
        var fl = new Line
        {
            X1 = padL, Y1 = Y(floor), X2 = W - padR, Y2 = Y(floor),
            Stroke = new SolidColorBrush(Color.FromRgb(0x3D, 0xBE, 0x7B)) { Opacity = 0.55 },
            StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 3, 3 },
        };
        c.Children.Add(cl);
        c.Children.Add(fl);

        // 曲线（SPEC §1 行为的静态映射：中电平线性 / ceiling 上方限幅（软拐 3dB）/ floor 下方上抬封顶 maxBoost）
        var pts = new PointCollection();
        for (int i = 0; i <= 160; i++)
        {
            double xin = -80.0 + i * 0.5;
            double y = Transfer(xin, ceiling, floor, maxBoost);
            pts.Add(new Point(X(xin), Y(y)));
        }
        c.Children.Add(new Polyline
        {
            Points = pts,
            Stroke = new SolidColorBrush(Color.FromRgb(0xE8, 0x3C, 0x3C)),
            StrokeThickness = 2.5, StrokeLineJoin = PenLineJoin.Round,
        });
    }

    /// <summary>下压 + 上抬的静态传递（软拐点 3dB / 上抬过渡带宽 6dB，对应 SPEC §1.1/§1.2）。</summary>
    internal static double Transfer(double x, double ceiling, double floor, double maxBoost)
    {
        double y = x;
        // 上抬链：低于 floor 提升，总增益 ≤ maxBoost；floor 上方 6dB 内线性混合
        if (x < floor)
        {
            double up = Math.Min(x + maxBoost, floor);
            y = up;
        }
        else if (x < floor + 6)
        {
            double up = Math.Min(x + maxBoost, floor);
            double t = (x - floor) / 6.0;
            y = up * (1 - t) + x * t;
        }
        // 下压链：ceiling 下方 3dB 软拐渐进，之上压回 ceiling
        if (x > ceiling)
        {
            y = Math.Min(y, ceiling);
        }
        else if (x > ceiling - 3)
        {
            double t = (x - (ceiling - 3)) / 3.0;
            y = y + (ceiling - y) * t * 0.5;
        }
        return Math.Clamp(y, -80, 0);
    }
}
