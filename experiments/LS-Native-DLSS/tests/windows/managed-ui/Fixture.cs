// Source-owned stand-in: validates injected hooks and real WPF/XML behavior.
// This is not Lossless Scaling and proves no NVIDIA/VRR hardware behavior.
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using System.Xml.Serialization;

namespace UI {
public class Profile {
    public enum FrameGenerationEnum { Off, LSFG3, LSFG2, LSFG1, LSFI, LSFI1 }
    public FrameGenerationEnum FrameGeneration { get; set; }
    public bool GsyncSupport { get; set; } = true;
    public bool ClipCursor { get; set; } = true;
    public float LSFG3Multiplier { get; set; } = 3;
    public int LSFG3Mode { get; set; } = 1;
    public Profile() { }
    public Profile(string title, string path, bool auto, int delay, Profile copy) {
        if (copy != null) { FrameGeneration = copy.FrameGeneration; GsyncSupport = copy.GsyncSupport; ClipCursor = copy.ClipCursor; LSFG3Multiplier = copy.LSFG3Multiplier; LSFG3Mode = copy.LSFG3Mode; }
    }
}
public class MainWindow : Window {
    public Profile Selected;
    public int SaveCount;
    public MainWindow(Profile p) { Selected = p; }
    public Profile GetSelectedProfile() { return Selected; }
    public void ProfileChanged() { ++SaveCount; }
    public void ApplyProfileToCore(Profile p) { LastNativeType = (int)p.FrameGeneration; }
    public int LastNativeType;
}
}
namespace UI.Pages {
public class ProfilePage : Page {
    public Border FrameGenerationCard;
    public ComboBox FrameGeneration;
    public StackPanel LSFG2Container, LSFG3Container;
    public Grid LSFGFlowScaleContainer, LSFGPerformanceContainer;
    readonly UI.Profile profile;
    public int NativeChanges;
    public ProfilePage(UI.Profile p) {
        profile = p; InitializeComponent(); Loaded += (s, e) => ApplyProfileToUI(profile);
    }
    public void InitializeComponent() {
        FrameGeneration = new ComboBox();
        foreach (string name in new[] { "Off", "LSFG3", "LSFG2", "LSFG1" }) FrameGeneration.Items.Add(new ComboBoxItem { Content = name });
        FrameGeneration.SelectionChanged += FrameGeneration_SelectionChanged;
        // Real BAML can select an initial item while InitializeComponent is
        // still constructing the page, before the injected Attach hook runs.
        // Our old fixture never raised this startup event.
        FrameGeneration.SelectedIndex = 0;
        LSFG2Container = new StackPanel(); LSFG3Container = new StackPanel(); LSFGFlowScaleContainer = new Grid(); LSFGPerformanceContainer = new Grid();
        var panel = new StackPanel(); panel.Children.Add(FrameGeneration); panel.Children.Add(LSFG2Container); panel.Children.Add(LSFG3Container); panel.Children.Add(LSFGFlowScaleContainer); panel.Children.Add(LSFGPerformanceContainer);
        FrameGenerationCard = new Border { Child = panel }; Content = FrameGenerationCard;
    }
    public void ApplyProfileToUI(UI.Profile p) {
        FrameGeneration.SelectedIndex = (int)p.FrameGeneration;
        FrameGeneration_SelectionChanged(null, null);
    }
    public void FrameGeneration_SelectionChanged(object sender, SelectionChangedEventArgs e) {
        if (!IsLoaded) return;
        ++NativeChanges;
        LSFG2Container.Visibility = FrameGeneration.SelectedIndex == 2 ? Visibility.Visible : Visibility.Collapsed;
        LSFG3Container.Visibility = FrameGeneration.SelectedIndex == 1 ? Visibility.Visible : Visibility.Collapsed;
        var w = (UI.MainWindow)Window.GetWindow(this);
        w.GetSelectedProfile().FrameGeneration = (UI.Profile.FrameGenerationEnum)FrameGeneration.SelectedIndex;
        w.ProfileChanged();
    }
}
}
namespace Fixture {
static class Tests {
    static void Check(bool result, string reason) { if (!result) throw new Exception(reason); }
    static void Set(object p, string name, object value) { p.GetType().GetProperty("NativeDLSS" + name).SetValue(p, value); }
    static object Get(object p, string name) { return p.GetType().GetProperty("NativeDLSS" + name).GetValue(p); }
    [STAThread] static int Main() { try { Run(); return 0; } catch (Exception e) { Console.Error.WriteLine(e); return 1; } }
    static void Run() {
        // Entry point's injected LoadFrom must make the helper resolvable first.
        Check(AppDomain.CurrentDomain.GetAssemblies().Any(a => a.GetName().Name == "NativeDLSS.UI"), "Helper bootstrap failed");
        Check(Enum.GetName(typeof(UI.Profile.FrameGenerationEnum), 6) == "DLSS", "DLSS enum absent");
        var app = new Application();
        var p = new UI.Profile { FrameGeneration = (UI.Profile.FrameGenerationEnum)6 };
        Set(p, "FlowPreset", 3); Set(p, "FlowGrid", 2); Set(p, "AnalysisPercent", 75); Set(p, "Slots", 4); Set(p, "DuplicateDelta", 1); Set(p, "HUD1", "1000,2000,3000,4000");
        var page = new UI.Pages.ProfilePage(p); var w = new UI.MainWindow(p) { Content = page, Width = 680, Height = 850, ShowInTaskbar = false };
        w.Show(); page.Dispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        Check(page.IsLoaded && page.FrameGeneration.Items.Count == 5, "WPF page or DLSS item missing");
        Check(page.FrameGeneration.SelectedIndex == 4 && (int)p.FrameGeneration == 6 && w.SaveCount == 0, "Load corrupted profile type or autosaved");
        var wrapper = (StackPanel)page.FrameGenerationCard.Child; var own = (StackPanel)wrapper.Children[1];
        Check(own.Visibility == Visibility.Visible && page.LSFG3Container.Visibility == Visibility.Collapsed, "DLSS card/native visibility");
        var choices = own.Children.OfType<DockPanel>().SelectMany(r => r.Children.OfType<ComboBox>()).ToArray();
        Check(choices.Length == 3 && choices[0].SelectedIndex == 2 && choices[1].SelectedIndex == 1 && choices[2].SelectedIndex == 2, "Saved Optical Flow quality not loaded");
        choices[0].SelectedIndex = 0; Check((int)Get(p,"FlowPreset") == 1 && w.SaveCount == 1, "Quality did not save through ProfileChanged");
        var expander = own.Children.OfType<Expander>().Single(); var advanced = (StackPanel)expander.Content;
        var boxes = advanced.Children.OfType<DockPanel>().SelectMany(r => r.Children.OfType<TextBox>()).ToArray();
        Check(boxes.Length == 11 && boxes[3].Text == "10,20,30,40", "HUD percent conversion failed");
        boxes[3].Text = "10,20,5,40";
        advanced.Children.OfType<Button>().Single().RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check((string)Get(p,"HUD1") == "1000,2000,3000,4000" && w.SaveCount == 1, "Invalid HUD mutated profile");
        boxes[3].Text = ""; boxes[0].Text = "0";
        advanced.Children.OfType<Button>().Single().RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check((string)Get(p,"HUD1") == "" && (int)Get(p,"DuplicateDelta") == 1 && w.SaveCount == 2, "Empty override/exact duplicate tolerance failed");
        var serializer = new XmlSerializer(typeof(UI.Profile)); string xml;
        using (var writer = new StringWriter()) { serializer.Serialize(writer,p); xml = writer.ToString(); }
        Check(xml.Contains("<FrameGeneration>DLSS</FrameGeneration>"), "DLSS not serialized by original enum name");
        UI.Profile round; using (var reader = new StringReader(xml)) round = (UI.Profile)serializer.Deserialize(reader);
        Check((int)round.FrameGeneration == 6 && (int)Get(round,"FlowPreset") == 1 && (string)Get(round,"HUD1") == "", "XML quality/HUD/type round trip failed");
        var copy = new UI.Profile("copy","",false,0,round);
        Check((int)Get(copy,"FlowGrid") == 2 && (int)Get(copy,"Slots") == 4 && (string)Get(copy,"HUD1") == "", "Profile copy lost DLSS fields");
        Check((int)Get(new UI.Profile("new","",false,0,null),"FlowPreset") == 0, "Null copy branch invalid");
        w.ApplyProfileToCore(p); // Runs actual P/Invoke bridge, no GPU target executed.
        Check(w.LastNativeType == 6, "Apply hook rewrote persistent FG type");
        page.FrameGeneration.SelectedIndex = 1;
        Check((int)p.FrameGeneration == 1 && page.NativeChanges == 1 && own.Visibility == Visibility.Collapsed && page.LSFG3Container.Visibility == Visibility.Visible, "Native LS selection handler not restored");
        Check(p.GsyncSupport && p.ClipCursor && p.LSFG3Multiplier == 3 && p.LSFG3Mode == 1, "Native cursor/VRR/multiplier values changed");
        w.ApplyProfileToCore(p);
        page.FrameGeneration.SelectedIndex = 4;
        Check((int)p.FrameGeneration == 6 && page.NativeChanges == 1 && own.Visibility == Visibility.Visible, "DLSS reselection ran native handler");
        // Switch profiles with DLSS already selected: suppressed SelectionChanged.
        var q = new UI.Profile { FrameGeneration = (UI.Profile.FrameGenerationEnum)6 }; Set(q,"FlowPreset",2); w.Selected = q;
        int saves = w.SaveCount; page.ApplyProfileToUI(q);
        Check(w.SaveCount == saves && choices[0].SelectedIndex == 1 && (int)q.FrameGeneration == 6, "Profile switch corrupted selection/settings");
        q.FrameGeneration = UI.Profile.FrameGenerationEnum.LSFG2; page.ApplyProfileToUI(q);
        Check(w.SaveCount == saves && page.LSFG2Container.Visibility == Visibility.Visible && own.Visibility == Visibility.Collapsed, "Native profile load left DLSS visibility or saved during loading");
        page.ApplyProfileToUI(p); w.Selected = p;
        w.Close(); app.Shutdown();
        Console.WriteLine("Real WPF (.NET " + Environment.Version + "): injected helper bootstrap, original item indices, DLSS/native transitions, quality/HUD validation, XML round trip, copy/null copy, profile load guard and cursor/VRR settings preservation passed.");
    }
}
}
