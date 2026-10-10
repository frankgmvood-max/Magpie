using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;

namespace NativeDLSS.UI {
public static class Controls {
    const int Dlss = 6;
    const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    static readonly ConditionalWeakTable<object, View> Views = new ConditionalWeakTable<object, View>();
    [StructLayout(LayoutKind.Sequential)]
    public struct UiSettings {
        public uint Version, Size, Type, Preset, Grid, Analysis, Slots, Duplicate, Scene, DuplicateDelta, CutDelta, CutPercent;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public uint[] Hud;
    }
    [DllImport("Lossless.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    static extern int ZNativeDLSSConfigure(ref UiSettings settings);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetPrivateProfileInt(string section, string key, int fallback, string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetPrivateProfileString(string section, string key, string fallback, StringBuilder value, uint size, string path);
    static readonly string Ini = Path.Combine(AppContext.BaseDirectory, "NativeDLSS.ini");
    static object Field(object obj, string name) { return obj.GetType().GetField(name, Flags).GetValue(obj); }
    static object Get(object obj, string name) { return obj.GetType().GetProperty(name, Flags).GetValue(obj); }
    static void Set(object obj, string name, object value) {
        var p = obj.GetType().GetProperty(name, Flags);
        p.SetValue(obj, p.PropertyType.IsEnum ? Enum.ToObject(p.PropertyType, value) : value);
    }
    static int IniInt(string key, int fallback) {
        string profile = IniString("NativeDLSS", "ActiveProfile", "");
        int global = unchecked((int)GetPrivateProfileInt("NativeDLSS", key, fallback, Ini));
        return profile.Length == 0 ? global : unchecked((int)GetPrivateProfileInt("Profile:" + profile, key, global, Ini));
    }
    static string IniString(string section, string key, string fallback) {
        var value = new StringBuilder(512); GetPrivateProfileString(section, key, fallback, value, 512, Ini); return value.ToString();
    }
    static string IniHud(int index) {
        string key = "HUD" + index, profile = IniString("NativeDLSS", "ActiveProfile", "");
        string global = IniString("NativeDLSS", key, "");
        return profile.Length == 0 ? global : IniString("Profile:" + profile, key, global);
    }
    static int Value(object profile, string key, int fallback, int low, int high) {
        int v = Convert.ToInt32(Get(profile, "NativeDLSS" + key));
        if (v == 0) v = IniInt(key, fallback);
        return Math.Max(low, Math.Min(high, v));
    }
    static UiSettings Settings(object profile) {
        int quality = IniInt("Quality", 2), legacyPreset = quality == 1 ? 1 : quality == 3 || quality == 5 ? 3 : 2;
        int preset = Value(profile, "FlowPreset", legacyPreset, 0, 3);
        if (preset == 0) preset = legacyPreset;
        int grid = Value(profile, "FlowGrid", quality >= 4 ? 2 : 4, 0, 4);
        if (grid == 0) grid = quality >= 4 ? 2 : 4;
        var s = new UiSettings {
            Version = 1, Size = 176, Type = Convert.ToUInt32(Get(profile, "FrameGeneration")), Preset = (uint)preset,
            Grid = grid == 2 ? 2u : 4u, Analysis = (uint)Value(profile, "AnalysisPercent", 50, 10, 100),
            Slots = (uint)Value(profile, "Slots", 3, 2, 4),
            // Profile 0 means inheritance, 1 means on, 2 means off. Legacy INI is 0/1.
            Duplicate = Toggle(profile, "DuplicateFilter"), Scene = Toggle(profile, "SceneCut"),
            DuplicateDelta = DuplicateTolerance(profile),
            CutDelta = (uint)Value(profile, "CutDelta", 64, 1, 255), CutPercent = (uint)Value(profile, "CutPercent", 65, 1, 100),
            Hud = new uint[32]
        };
        for (int i = 0; i < 8; ++i) {
            string text = (string)Get(profile, "NativeDLSSHUD" + (i + 1)) ?? IniHud(i + 1);
            uint[] rect;
            if (!TryRect(text, out rect)) throw new InvalidDataException("HUD " + (i + 1) + ": нужны x1,y1,x2,y2 в пределах 0…100%.");
            Array.Copy(rect, 0, s.Hud, i * 4, 4);
        }
        return s;
    }
    static uint Toggle(object profile, string key) {
        int v = Convert.ToInt32(Get(profile, "NativeDLSS" + key));
        return v == 0 ? (IniInt(key, 1) == 1 ? 1u : 0u) : v == 1 ? 1u : 0u;
    }
    static uint DuplicateTolerance(object profile) {
        int encoded = Convert.ToInt32(Get(profile, "NativeDLSSDuplicateDelta"));
        return (uint)Math.Max(0, Math.Min(8, encoded == 0 ? IniInt("DuplicateDelta", 0) : encoded - 1));
    }
    // Persistence and bridge use ten-thousandths; UI converts percentages.
    static bool TryRect(string text, out uint[] rect) {
        rect = new uint[4]; if (string.IsNullOrWhiteSpace(text)) return true;
        var parts = text.Split(','); if (parts.Length != 4) return false;
        for (int i = 0; i < 4; ++i) if (!uint.TryParse(parts[i].Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out rect[i]) || rect[i] > 10000) return false;
        return rect[0] < rect[2] && rect[1] < rect[3];
    }
    public static void Attach(object page) {
        if (Views.TryGetValue(page, out _)) return;
        var combo = (ComboBox)Field(page, "FrameGeneration");
        var v = new View(page, combo); Views.Add(page, v);
        combo.Items.Add(new ComboBoxItem { Content = "DLSS" });
        // REA/Cecil identify this as a Border, rather than a Wpf.Ui CardExpander.
        var card = (Border)Field(page, "FrameGenerationCard");
        var original = card.Child; card.Child = null;
        var wrapper = new StackPanel(); wrapper.Children.Add(original); wrapper.Children.Add(v.Panel); card.Child = wrapper;
    }
    public static void BeginProfile(object page, object profile) {
        var v = Views.GetValue(page, p => throw new InvalidOperationException("DLSS controls were not attached."));
        v.Loading = true; v.Profile = profile; v.Load();
    }
    public static int TypeIndex(int type, object page) {
        int index = Views.GetValue(page, p => throw new InvalidOperationException()).Index;
        return type == Dlss ? index : type >= 0 && type < index ? type : -1;
    }
    public static void EndProfile(object page) { var v = Views.GetValue(page, p => throw new InvalidOperationException()); v.Loading = false; v.Visibility(); }
    public static bool HandleType(object page) {
        // BAML wires SelectionChanged, then assigns SelectedIndex=0 inside
        // InitializeComponent. Attach runs only when that call returns.
        // Preserve the original handler's IsLoaded guard for this early event.
        if (!Views.TryGetValue(page, out var v)) return false;
        if (v.Loading) return true;
        v.Visibility();
        if (v.Combo.SelectedIndex != v.Index) return false;
        if (((Page)page).IsLoaded && v.Profile != null) {
            Set(v.Profile, "FrameGeneration", Dlss); v.Changed();
        }
        return true;
    }
    public static void ApplyProfile(object profile) {
        try {
            var s = Convert.ToInt32(Get(profile, "FrameGeneration")) == Dlss ? Settings(profile) : new UiSettings { Version = 1, Size = 176, Type = Convert.ToUInt32(Get(profile, "FrameGeneration")), Preset = 2, Grid = 4, Analysis = 50, Slots = 3, Duplicate = 1, Scene = 1, CutDelta = 64, CutPercent = 65, Hud = new uint[32] };
            if (Marshal.SizeOf(typeof(UiSettings)) != 176 || ZNativeDLSSConfigure(ref s) != 1)
                throw new InvalidOperationException("Native DLSS: backend отклонил настройки.");
        } catch (Exception e) {
            Log(e);
            // Do not silently save a DLSS selection while running native LSFG.
            throw new InvalidOperationException("Не удалось применить DLSS. Проверьте установку NativeDLSS и logs/native-ui.log.", e);
        }
    }
    static void Log(Exception e) {
        try { string folder = Path.Combine(AppContext.BaseDirectory, "logs"); Directory.CreateDirectory(folder); File.AppendAllText(Path.Combine(folder, "native-ui.log"), DateTime.UtcNow.ToString("O") + " " + e + Environment.NewLine); } catch (IOException) { }
    }
    sealed class View {
        public readonly object Page;
        public readonly ComboBox Combo;
        public readonly int Index;
        public readonly StackPanel Panel = new StackPanel { Margin = new Thickness(16, 8, 16, 12), Visibility = System.Windows.Visibility.Collapsed };
        public object Profile;
        public bool Loading;
        readonly ComboBox Preset = Choice("FAST", "MEDIUM", "SLOW"), Grid = Choice("4 × 4", "2 × 2"), Slots = Choice("2", "3", "4");
        readonly Slider Analysis = new Slider { Minimum = 10, Maximum = 100, TickFrequency = 5, IsSnapToTickEnabled = true, MinWidth = 160 };
        readonly TextBlock AnalysisText = new TextBlock();
        readonly CheckBox Duplicate = new CheckBox { Content = "Пропуск одинаковых кадров" }, Scene = new CheckBox { Content = "Сброс при смене сцены" };
        readonly TextBox DuplicateDelta = new TextBox(), CutDelta = new TextBox(), CutPercent = new TextBox();
        readonly TextBox[] Hud = new TextBox[8];
        readonly TextBlock Status = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0) };
        public View(object page, ComboBox combo) {
            Page = page; Combo = combo; Index = combo.Items.Count;
            Panel.Children.Add(new TextBlock { Text = "DLSS Frame Generation ×2 • NVIDIA Optical Flow • SDR", TextWrapping = TextWrapping.Wrap, FontWeight = FontWeights.SemiBold });
            Row(Panel, "Optical Flow: качество", Preset); Row(Panel, "Optical Flow: сетка", Grid);
            Row(Panel, "Разрешение анализа", Analysis); Panel.Children.Add(AnalysisText); Row(Panel, "Кадры в обработке", Slots);
            Panel.Children.Add(new TextBlock { Text = "Изменения качества Optical Flow вступают в силу после полного перезапуска LS.", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 8), Opacity = .8 });
            Panel.Children.Add(Duplicate); Panel.Children.Add(Scene);
            var advanced = new StackPanel(); Row(advanced, "Допуск одинаковых кадров (0–8)", DuplicateDelta); Row(advanced, "Порог смены сцены (1–255)", CutDelta); Row(advanced, "Изменившаяся область сцены (%)", CutPercent);
            advanced.Children.Add(new TextBlock { Text = "HUD: x1,y1,x2,y2 в процентах; пустая строка отключает область. В этих областях используется исходное изображение.", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 8) });
            for (int i = 0; i < 8; ++i) { Hud[i] = new TextBox(); Row(advanced, "HUD " + (i + 1), Hud[i]); }
            var save = new Button { Content = "Сохранить параметры фильтров и HUD", Margin = new Thickness(0, 8, 0, 0) }; save.Click += (s, e) => SaveAdvanced(); advanced.Children.Add(save);
            Panel.Children.Add(new Expander { Header = "Фильтры и защита HUD", Content = advanced, Margin = new Thickness(0, 8, 0, 0) }); Panel.Children.Add(Status);
            Preset.SelectionChanged += (s, e) => SaveQuality(); Grid.SelectionChanged += (s, e) => SaveQuality(); Slots.SelectionChanged += (s, e) => SaveQuality();
            Analysis.ValueChanged += (s, e) => { AnalysisText.Text = ((int)Analysis.Value) + "% от исходного разрешения"; SaveQuality(); };
            Duplicate.Click += (s, e) => SaveQuality(); Scene.Click += (s, e) => SaveQuality();
        }
        static ComboBox Choice(params string[] values) { var c = new ComboBox { MinWidth = 140 }; foreach (string s in values) c.Items.Add(s); return c; }
        static void Row(Panel parent, string label, FrameworkElement control) {
            var row = new DockPanel { Margin = new Thickness(0, 5, 0, 5), LastChildFill = true };
            var text = new TextBlock { Text = label, Width = 205, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center };
            DockPanel.SetDock(text, Dock.Left); row.Children.Add(text); row.Children.Add(control); parent.Children.Add(row);
        }
        public void Load() {
            try {
                var s = Settings(Profile); Preset.SelectedIndex = (int)s.Preset - 1; Grid.SelectedIndex = s.Grid == 2 ? 1 : 0; Slots.SelectedIndex = (int)s.Slots - 2; Analysis.Value = s.Analysis;
                Duplicate.IsChecked = s.Duplicate == 1; Scene.IsChecked = s.Scene == 1;
                DuplicateDelta.Text = s.DuplicateDelta.ToString(CultureInfo.InvariantCulture); CutDelta.Text = s.CutDelta.ToString(CultureInfo.InvariantCulture); CutPercent.Text = s.CutPercent.ToString(CultureInfo.InvariantCulture);
                for (int i = 0; i < 8; ++i) { var a = new string[4]; for (int j = 0; j < 4; ++j) a[j] = (s.Hud[i * 4 + j] / 100m).ToString(CultureInfo.InvariantCulture); Hud[i].Text = s.Hud[i * 4 + 2] == 0 ? "" : string.Join(",", a); }
                Status.Text = "";
            } catch (Exception e) { Status.Text = e.Message; Log(e); }
        }
        public void Visibility() {
            bool dlss = Combo.SelectedIndex == Index;
            Panel.Visibility = dlss ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
            int native = dlss ? -1 : Combo.SelectedIndex;
            ((UIElement)Field(Page,"LSFG2Container")).Visibility = native == 2 ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
            ((UIElement)Field(Page,"LSFG3Container")).Visibility = native == 1 ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
            foreach (string name in new[] { "LSFGFlowScaleContainer", "LSFGPerformanceContainer" }) ((UIElement)Field(Page,name)).Visibility = native == 1 || native == 2 ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
        }
        public void Changed() {
            var window = Window.GetWindow((DependencyObject)Page);
            if (window == null) return;
            try { window.GetType().GetMethod("ProfileChanged", Flags).Invoke(window, null); Status.Text = "Сохранено в профиле LS."; }
            catch (Exception e) { var cause = e is TargetInvocationException && e.InnerException != null ? e.InnerException : e; Status.Text = cause.Message; Log(cause); }
        }
        void SaveQuality() {
            if (Loading || Profile == null || !((Page)Page).IsLoaded) return;
            Set(Profile, "NativeDLSSFlowPreset", Preset.SelectedIndex + 1); Set(Profile, "NativeDLSSFlowGrid", Grid.SelectedIndex == 1 ? 2 : 4);
            Set(Profile, "NativeDLSSAnalysisPercent", (int)Analysis.Value); Set(Profile, "NativeDLSSSlots", Slots.SelectedIndex + 2);
            Set(Profile, "NativeDLSSDuplicateFilter", Duplicate.IsChecked == true ? 1 : 2); Set(Profile, "NativeDLSSSceneCut", Scene.IsChecked == true ? 1 : 2); Changed();
        }
        void SaveAdvanced() {
            try {
                int dup = Parse(DuplicateDelta.Text, 0, 8), delta = Parse(CutDelta.Text, 1, 255), percent = Parse(CutPercent.Text, 1, 100);
                var values = new string[8];
                for (int i = 0; i < 8; ++i) {
                    if (string.IsNullOrWhiteSpace(Hud[i].Text)) { values[i] = ""; continue; }
                    var parts = Hud[i].Text.Split(','); if (parts.Length != 4) throw new FormatException("HUD " + (i + 1) + ": нужны четыре координаты.");
                    var converted = new string[4];
                    for (int j = 0; j < 4; ++j) { decimal n; if (!decimal.TryParse(parts[j], NumberStyles.AllowDecimalPoint | NumberStyles.AllowLeadingWhite | NumberStyles.AllowTrailingWhite, CultureInfo.InvariantCulture, out n) || n < 0 || n > 100) throw new FormatException("Координаты HUD: 0…100, десятичный разделитель — точка."); converted[j] = decimal.Round(n * 100, 0).ToString(CultureInfo.InvariantCulture); }
                    values[i] = string.Join(",", converted); uint[] rect; if (!TryRect(values[i], out rect)) throw new FormatException("HUD " + (i + 1) + ": правая и нижняя границы должны быть больше левой и верхней.");
                }
                // Validate every value before mutating the actual profile.
                Set(Profile, "NativeDLSSDuplicateDelta", dup + 1); Set(Profile, "NativeDLSSCutDelta", delta); Set(Profile, "NativeDLSSCutPercent", percent);
                for (int i = 0; i < 8; ++i) Set(Profile, "NativeDLSSHUD" + (i + 1), values[i]); Changed();
            } catch (Exception e) { Status.Text = e.Message; }
        }
        static int Parse(string text, int low, int high) { int n; if (!int.TryParse(text, out n) || n < low || n > high) throw new FormatException("Значение должно быть в пределах " + low + "…" + high + "."); return n; }
    }
}
}
