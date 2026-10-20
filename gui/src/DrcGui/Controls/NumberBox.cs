using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;

namespace DrcGui.Controls;

/// <summary>
/// 可直接输入的数字框（2026-10-15 报障④：数值只能拖、不能点着改）。
/// 外观 = 原来的数值文本；**单击即进入编辑**（全选），不改变页面观感。
/// 交互：Enter / 失焦 = 提交；Esc = 撤销；↑↓ = ±1 步进（Shift = ×10）；滚轮 = ±1 步进（仅在获得焦点时，
/// 避免抢页面滚动）；右键 = 回 <see cref="Default"/>。
/// 提交一律「按 Step 吸附 + 钳位到 [Minimum, Maximum]」，所以**显示值 = 下发值**。
/// 与滑杆联动：拖滑杆时用 <see cref="Value"/> 写回（只刷显示，不触发 <see cref="Committed"/>，不会二次提交）。
/// </summary>
public sealed class NumberBox : UserControl
{
    static readonly Brush FocusBg = new SolidColorBrush(Color.FromRgb(0x2E, 0x2E, 0x36));
    static readonly Brush FocusBorder = new SolidColorBrush(Color.FromRgb(0x4A, 0x4A, 0x54));

    readonly Border _box = new()
    {
        CornerRadius = new CornerRadius(5),
        Padding = new Thickness(6, 1, 6, 1),
        Background = Brushes.Transparent,
        BorderThickness = new Thickness(1),
        BorderBrush = Brushes.Transparent,
    };

    readonly TextBox _tb = new()
    {
        Background = Brushes.Transparent,
        BorderThickness = new Thickness(0),
        Padding = new Thickness(0),
        TextAlignment = TextAlignment.Center,
        CaretBrush = Brushes.White,
        SelectionBrush = new SolidColorBrush(Color.FromRgb(0xC2, 0x0C, 0x0C)),
        MinWidth = 6,
    };

    double _value;
    double _min = double.NegativeInfinity, _max = double.PositiveInfinity;
    double _step, _default = double.NaN;
    int _decimals = 1;
    bool _plus;

    public NumberBox()
    {
        FontFamily = new FontFamily("Microsoft YaHei UI");
        FontSize = 16;
        FontWeight = FontWeights.SemiBold;
        Content = _box;
        _box.Child = _tb;
        // TextBox 的主题样式会自带 Foreground/字号 ⇒ 显式跟随本控件的 Foreground/Font*（父级 XAML 里设的红/绿/白、16/12px）
        _tb.SetBinding(TextBox.ForegroundProperty, new Binding(nameof(Foreground)) { Source = this });
        _tb.SetBinding(TextBox.FontSizeProperty, new Binding(nameof(FontSize)) { Source = this });
        _tb.SetBinding(TextBox.FontWeightProperty, new Binding(nameof(FontWeight)) { Source = this });
        _tb.SetBinding(TextBox.FontFamilyProperty, new Binding(nameof(FontFamily)) { Source = this });
        Cursor = Cursors.IBeam;
        ToolTip = "单击直接输入 · Enter 提交 · Esc 撤销 · ↑↓ 步进（Shift ×10）· 滚轮微调 · 右键恢复默认";

        _tb.GotKeyboardFocus += (_, _) => { RefreshChrome(); _tb.SelectAll(); };
        _tb.LostKeyboardFocus += (_, _) => { Commit(); RefreshChrome(); };
        _tb.PreviewKeyDown += OnKeyDown;
        _tb.PreviewMouseWheel += OnWheel;
        PreviewMouseRightButtonUp += OnRightClick;
        MouseEnter += (_, _) => RefreshChrome();
        MouseLeave += (_, _) => RefreshChrome();

        RefreshText();
    }

    /// <summary>用户提交生效值（≠ 滑杆联动写回）。</summary>
    public event Action<double>? Committed;

    public double Minimum { get => _min; set { _min = value; SnapBack(); } }
    public double Maximum { get => _max; set { _max = value; SnapBack(); } }
    /// <summary>步进；0 = 不吸附（连续值）。</summary>
    public double Step { get => _step; set { _step = value; SnapBack(); } }
    public int Decimals { get => _decimals; set { _decimals = Math.Max(0, value); RefreshText(); } }
    /// <summary>单位后缀（"dB" / "ms" / ""），只在非编辑态显示。</summary>
    public string Unit
    {
        get => _unit;
        set { _unit = value ?? ""; RefreshText(); }
    }
    string _unit = "";
    /// <summary>正数是否显示 "+"（EQ 增益用）。</summary>
    public bool ShowPlusSign { get => _plus; set { _plus = value; RefreshText(); } }
    /// <summary>右键恢复的默认值；NaN = 该框不支持（右键无动作）。</summary>
    public double Default { get => _default; set => _default = value; }

    /// <summary>写回显示值（滑杆联动 / 启动加载用）。不触发 <see cref="Committed"/>。</summary>
    public double Value
    {
        get => _value;
        set { _value = Snap(value); RefreshText(); }
    }

    void SnapBack() => Value = _value;

    double Snap(double v)
    {
        if (double.IsNaN(v) || double.IsInfinity(v)) v = _value;
        v = Math.Clamp(v, _min, _max);
        if (_step > 0) v = Math.Round(v / _step, MidpointRounding.AwayFromZero) * _step;
        v = Math.Round(v, _decimals);          // 去掉 0.30000000000000004 这类尾巴
        return Math.Clamp(v, _min, _max);
    }

    string Format(double v)
    {
        string s = v.ToString("F" + _decimals, CultureInfo.InvariantCulture);
        if (_plus && v > 0) s = "+" + s;
        return _unit.Length > 0 ? s + " " + _unit : s;
    }

    void RefreshText()
    {
        if (_tb.IsKeyboardFocusWithin) return;   // 正在打字 ⇒ 不覆盖用户输入
        _tb.Text = Format(_value);
    }

    void RefreshChrome()
    {
        bool focus = _tb.IsKeyboardFocusWithin;
        bool hover = IsMouseOver;
        _box.Background = focus ? FocusBg : Brushes.Transparent;
        _box.BorderBrush = focus ? FocusBorder : hover ? FocusBorder : Brushes.Transparent;
    }

    bool TryParse(string text, out double v)
    {
        v = 0;
        string s = (text ?? "").Replace(',', '.').Trim();
        if (_unit.Length > 0 && s.EndsWith(_unit, StringComparison.OrdinalIgnoreCase))
            s = s[..^_unit.Length].Trim();
        s = s.Replace("+", "").Replace("dB", "").Replace("ms", "").Trim();
        return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v);
    }

    /// <summary>把编辑框里的内容提交为生效值（Enter / 失焦 / 步进前调用）。</summary>
    void Commit()
    {
        if (!TryParse(_tb.Text, out double typed)) { RefreshText(); return; }
        CommitValue(typed);
    }

    void CommitValue(double raw)
    {
        double v = Snap(raw);
        bool changed = Math.Abs(v - _value) > 1e-9;
        _value = v;
        RefreshText();
        if (changed) Committed?.Invoke(v);
    }

    void OnKeyDown(object sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Enter:
                Commit();
                _tb.SelectAll();
                e.Handled = true;
                break;
            case Key.Escape:
                RefreshText();
                e.Handled = true;
                break;
            case Key.Up:
            case Key.Down:
                Commit();                                   // 先把已输入的数字落地，再步进
                double d = _step > 0 ? _step : 0.1;
                if ((Keyboard.Modifiers & ModifierKeys.Shift) != 0) d *= 10;
                CommitValue(_value + (e.Key == Key.Up ? d : -d));
                e.Handled = true;
                break;
        }
    }

    void OnWheel(object sender, MouseWheelEventArgs e)
    {
        if (!_tb.IsKeyboardFocusWithin) return;             // 未聚焦 ⇒ 让页面滚动照常
        double d = _step > 0 ? _step : 0.1;
        if ((Keyboard.Modifiers & ModifierKeys.Shift) != 0) d *= 10;
        CommitValue(_value + (e.Delta > 0 ? d : -d));
        e.Handled = true;
    }

    void OnRightClick(object sender, MouseButtonEventArgs e)
    {
        if (double.IsNaN(_default)) return;
        CommitValue(_default);
        e.Handled = true;
    }
}
