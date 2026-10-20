using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using DrcGui.Controls;
using DrcGui.Core;

namespace DrcGui.Pages;

public partial class EqPage : UserControl
{
    // 预设注释（TASK-014 按外部资料重做）：
    //   脚步增强 = **削** 31/62Hz（低频嗡鸣会加剧"楼上/楼下"误判）+ **抬** 1k–4kHz
    //              （脚步的"定义"与方向线索在 2.5–3.5kHz），8k 略降防刺耳
    //   人声     = 250Hz–4kHz 抬 +1~+3.5dB（可懂度区），两端平直
    //   还原     = 全 0（直通）
    static readonly double[] PresetFootstep = { -3, -3, -1, 0.5, 1.5, 2.5, 3.5, 3.5, 1, -1 };
    static readonly double[] PresetVoice = { 0, 0, 0, 1, 3, 3.5, 2, 1, 0, 0 };

    sealed class BandVm
    {
        public double Freq;
        public double Gain;
        public event Action? UserChanged;
        public event PropertyChangedEventHandler? PropChanged;

        public string GainText => Gain >= 0 ? $"+{Gain:F1}" : $"{Gain:F1}";
        public string FreqText => Freq >= 1000 ? $"{Freq / 1000:0}k" : $"{Freq:0}";

        public void SetGain(double g)
        {
            g = Math.Clamp(g, -12, 12);
            if (Math.Abs(Gain - g) < 0.001) return;
            Gain = g;
            PropChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(GainText)));
            UserChanged?.Invoke();
        }
        public void SyncText() => PropChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(GainText)));
    }

    readonly BandVm[] _bands = new BandVm[DrcState.EqBands];
    readonly Slider[] _sliders = new Slider[DrcState.EqBands];
    bool _updating;
    bool _dragging;

    public EqPage()
    {
        InitializeComponent();
        BuildBands();
        Loaded += (_, _) => RedrawCurve();
        App.State.ParamsChanged += OnParamsChanged;
        Unloaded += (_, _) => App.State.ParamsChanged -= OnParamsChanged;
    }

    void BuildBands()
    {
        for (int i = 0; i < DrcState.EqBands; i++)
        {
            int idx = i;
            var vm = new BandVm { Freq = DrcState.EqFreqs[i], Gain = App.State.EqGainDb[i] };
            vm.UserChanged += () =>
            {
                if (_updating) return;
                App.State.EqGainDb[idx] = vm.Gain;
                MarkCustom();
                App.State.Apply();
            };
            _bands[i] = vm;

            var val = new NumberBox
            {
                Minimum = -12, Maximum = 12, Step = 0.5, Decimals = 1, ShowPlusSign = true, Default = 0,
                FontSize = 12, HorizontalAlignment = HorizontalAlignment.Center, Value = vm.Gain,
            };
            val.Committed += v => { vm.SetGain(v); RedrawCurve(); };
            vm.PropChanged += (_, _) => val.Value = vm.Gain;

            var sl = new Slider
            {
                Style = (Style)FindResource("VSliderEq"),
                Minimum = -12, Maximum = 12, Height = 150,
                TickFrequency = 0.5, IsSnapToTickEnabled = true,
                Margin = new Thickness(0, 8, 0, 0),
            };
            sl.Value = vm.Gain;
            sl.ValueChanged += (_, _) => { vm.SetGain(sl.Value); RedrawCurve(); };
            _sliders[i] = sl;

            var freq = new TextBlock
            {
                Text = vm.FreqText, FontSize = 11,
                Foreground = new SolidColorBrush(Color.FromRgb(0x6A, 0x6A, 0x72)),
                HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 8, 0, 0),
            };
            var panel = new StackPanel { Width = 54 };
            panel.Children.Add(val);
            panel.Children.Add(sl);
            panel.Children.Add(freq);
            BandsHost.Children.Add(panel);
        }
    }

    void MarkCustom()
    {
        _updating = true;
        if (CbPreset.SelectedIndex != 0) CbPreset.SelectedIndex = 0;
        _updating = false;
    }

    void OnPresetChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || CbPreset.SelectedIndex < 0) return;
        double[]? gain = CbPreset.SelectedIndex switch
        {
            1 => PresetFootstep,
            2 => PresetVoice,
            3 => new double[DrcState.EqBands],
            _ => null,
        };
        if (gain == null) return;
        _updating = true;
        for (int i = 0; i < DrcState.EqBands; i++)
        {
            App.State.EqGainDb[i] = gain[i];
            _bands[i].Gain = gain[i];
            _bands[i].SyncText();
            _sliders[i].Value = gain[i];
        }
        _updating = false;
        App.State.Apply();
    }

    void OnParamsChanged()
    {
        _updating = true;
        for (int i = 0; i < DrcState.EqBands; i++)
        {
            _bands[i].Gain = App.State.EqGainDb[i];
            _bands[i].SyncText();
            _sliders[i].Value = App.State.EqGainDb[i];
        }
        _updating = false;
        RedrawCurve();
    }

    // ===== 响应曲线绘制 =====
    void OnEqSizeChanged(object sender, SizeChangedEventArgs e) => RedrawCurve();

    void RedrawCurve()
    {
        var c = EqCanvas;
        if (c == null || c.ActualWidth < 40 || c.ActualHeight < 40) return;
        c.Children.Clear();
        double W = c.ActualWidth, H = c.ActualHeight;
        const double padL = 16, padR = 16, padT = 10, padB = 22;

        double X(double f) => padL + Math.Log10(f / 20.0) / 3.0 * (W - padL - padR);
        double Y(double db) => padT + (1.0 - (db + 12.0) / 24.0) * (H - padT - padB);

        foreach (double db in new[] { -12, -6, 0, 6, 12 })
        {
            var line = new Line
            {
                X1 = padL, Y1 = Y(db), X2 = W - padR, Y2 = Y(db),
                Stroke = new SolidColorBrush(Color.FromRgb(0x33, 0x33, 0x3A)),
                StrokeThickness = db == 0 ? 1.4 : 1,
                StrokeDashArray = db == 0 ? new DoubleCollection { 4, 4 } : new DoubleCollection(),
            };
            c.Children.Add(line);
        }
        for (int i = 0; i < DrcState.EqBands; i++)
        {
            double f = DrcState.EqFreqs[i];
            c.Children.Add(new Line
            {
                X1 = X(f), Y1 = padT, X2 = X(f), Y2 = H - padB,
                Stroke = new SolidColorBrush(Color.FromRgb(0x2E, 0x2E, 0x34)), StrokeThickness = 1,
            });
            var label = new TextBlock
            {
                Text = _bands[i].FreqText, FontSize = 10,
                Foreground = new SolidColorBrush(Color.FromRgb(0x6A, 0x6A, 0x72)),
            };
            Canvas.SetLeft(label, X(f) - 10); Canvas.SetTop(label, H - padB + 4);
            c.Children.Add(label);
        }

        double MagAt(double f)
        {
            double db = 0;
            for (int b = 0; b < DrcState.EqBands; b++)
                db += EqMath.MagDb(DrcState.EqFreqs[b], _bands[b].Gain, f);
            return db;
        }

        var pts = new PointCollection();
        const int N = 240;
        for (int i = 0; i <= N; i++)
        {
            double f = 20.0 * Math.Pow(1000.0, i / (double)N);
            pts.Add(new Point(X(f), Y(MagAt(f))));
        }
        c.Children.Add(new Polyline
        {
            Points = pts,
            Stroke = new SolidColorBrush(Color.FromRgb(0xE8, 0x3C, 0x3C)),
            StrokeThickness = 2.5, StrokeLineJoin = PenLineJoin.Round,
        });

        for (int b = 0; b < DrcState.EqBands; b++)
        {
            double f = DrcState.EqFreqs[b];
            var dot = new Ellipse
            {
                Width = 7, Height = 7, Fill = new SolidColorBrush(Color.FromRgb(0xE8, 0x3C, 0x3C)),
                Stroke = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x23)), StrokeThickness = 2,
            };
            Canvas.SetLeft(dot, X(f) - 3.5);
            Canvas.SetTop(dot, Y(MagAt(f)) - 3.5);
            c.Children.Add(dot);
        }
    }

    // ===== 曲线拖拽（双向联动：拖曲线 → 最近频段推子） =====
    void OnCanvasDown(object sender, MouseButtonEventArgs e)
    {
        _dragging = true;
        EqCanvas.CaptureMouse();
        DragApply(e.GetPosition(EqCanvas));
    }

    void OnCanvasMove(object sender, MouseEventArgs e)
    {
        if (_dragging) DragApply(e.GetPosition(EqCanvas));
    }

    void OnCanvasUp(object sender, MouseButtonEventArgs e)
    {
        _dragging = false;
        EqCanvas.ReleaseMouseCapture();
    }

    void DragApply(Point p)
    {
        var c = EqCanvas;
        const double padL = 16, padR = 16, padT = 10, padB = 22;
        double W = c.ActualWidth, H = c.ActualHeight;
        if (W < 40 || H < 40) return;
        double fx = Math.Clamp((p.X - padL) / (W - padL - padR), 0, 1);
        double freq = 20.0 * Math.Pow(1000.0, fx);
        double gain = 12.0 * (1.0 - 2.0 * Math.Clamp((p.Y - padT) / (H - padT - padB), 0, 1));

        // 最近频段（对数距离）
        int best = 0; double bestD = double.MaxValue;
        for (int i = 0; i < DrcState.EqBands; i++)
        {
            double d = Math.Abs(Math.Log10(DrcState.EqFreqs[i] / freq));
            if (d < bestD) { bestD = d; best = i; }
        }
        _sliders[best].Value = Math.Clamp(gain, -12, 12);
        RedrawCurve();
    }
}
