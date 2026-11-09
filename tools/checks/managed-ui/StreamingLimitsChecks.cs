using System;
using System.Globalization;
using System.IO;
using System.Reflection;
using Xenko.UI;
using Xenko.UI.Controls;
using Xenko.UI.Events;
using VdHsMod;

namespace Xenko.UI.Events { public class RoutedEventArgs : EventArgs { } }

namespace Xenko.UI.Controls
{
    public class Slider : UIElement
    {
        private float _maximum = 1, _value;
        public float Minimum { get; set; }
        public float Maximum
        {
            get { return _maximum; }
            set { MockGame.AssertUiWrite(); _maximum = value; Value = _value; }
        }
        public float Value
        {
            get { return _value; }
            set
            {
                MockGame.AssertUiWrite();
                float next = Math.Max(Minimum, Math.Min(Maximum, value));
                if (next == _value) return;
                _value = next;
                ValueChanged?.Invoke(this, new RoutedEventArgs());
            }
        }
        public event EventHandler<RoutedEventArgs> ValueChanged;
    }
    public class LimitSlider : Slider
    {
        private float _limit;
        public float Limit
        {
            get { return _limit; }
            set { MockGame.AssertUiWrite(); _limit = Math.Max(Minimum, Math.Min(Maximum, value)); }
        }
    }
}

namespace VirtualDesktop.Mobile
{
    public enum VRGraphicsQuality { Potato = -1, Low, Medium, High, Ultra, Godlike, Monster, Rampage, Unstoppable }
    public class SettingsBase<T> where T : new() { public static T Default { get; } = new T(); }
    public class SharedUserSettings : SettingsBase<SharedUserSettings>
    {
        private float _vr = 0.36f, _desktop = 0.17f;
        private VRGraphicsQuality _quality = VRGraphicsQuality.Medium;
        public int Writes;
        public float VRBitrateLimit { get { return _vr; } set { if (_vr != value) { _vr = value; Writes++; VRBitrateLimitChanged?.Invoke(this, EventArgs.Empty); } } }
        public float DesktopBitrateLimit { get { return _desktop; } set { if (_desktop != value) { _desktop = value; Writes++; DesktopBitrateLimitChanged?.Invoke(this, EventArgs.Empty); } } }
        public VRGraphicsQuality VRGraphicsQuality { get { return _quality; } set { if (_quality != value) { _quality = value; Writes++; VRGraphicsQualityChanged?.Invoke(this, EventArgs.Empty); } } }
        public event EventHandler VRBitrateLimitChanged, DesktopBitrateLimitChanged, VRGraphicsQualityChanged;
    }
    public class SharedMobileSettings : SettingsBase<SharedMobileSettings>
    {
        public int HmdType { get; set; } = 800;
        private int _bandwidth;
        public int MeasuredBandwidth { get { return _bandwidth; } set { _bandwidth = value; MeasuredBandwidthChanged?.Invoke(this, EventArgs.Empty); } }
        public bool FoveatedStreaming { get; set; }
        public event EventHandler MeasuredBandwidthChanged, FoveatedStreamingChanged;
        public void FoveationChanged() { FoveatedStreamingChanged?.Invoke(this, EventArgs.Empty); }
    }
    public class SharedStreamerSettings : SettingsBase<SharedStreamerSettings>
    {
        private int _codec = 5;
        public int ActiveCodec { get { return _codec; } set { _codec = value; ActiveCodecChanged?.Invoke(this, EventArgs.Empty); } }
        public event EventHandler ActiveCodecChanged;
    }
}

namespace Xenko.VR
{
    // Offline doubles of the inspected shared formulas, not a live encoder.
    public static class HmdResolutionTypeExtensions
    {
        public static int GetMinVRBitrate(int hmd) { return 10000000; }
        public static int GetMaxVRBitrate(int hmd, int codec, bool foveated) { return codec == 5 ? (foveated ? 200000000 : 400000000) : 150000000; }
        public static int GetMinDesktopBitrate(int hmd) { return 4000000; }
        public static int GetMaxDesktopBitrate(int hmd) { return 120000000; }
        public static int Rate(float value, bool vr)
        {
            value = (float)Math.Round(value, 3, MidpointRounding.AwayFromZero);
            int min = vr ? 10000000 : 4000000;
            int max = vr ? GetMaxVRBitrate(800, VirtualDesktop.Mobile.SharedStreamerSettings.Default.ActiveCodec, false) : 120000000;
            return (int)((1f - value) * min + value * max);
        }
    }
}

public class LimitsStreamingTab
{
    public Slider _sldQuality = new Slider { Minimum = -1, Maximum = 4, Value = 1 };
    public LimitSlider _sldBitrate = new LimitSlider { Value = 0.36f };
    public CheckBox _chkSuperResolution = new CheckBox { Content = new TextBlock { Font = "latin-font" } };
    public int Displayed;
    public int QualitySounds, QualityRefreshes, RestartQualityRefreshes;
    public string QualityLabel, LastSound;
    public bool ThrowQualityRefresh;
    public LimitsStreamingTab() { _sldQuality.ValueChanged += OnVRGraphicsQualityValueChanged; }
    // Match the inspected native handler: sound is a slider-event side effect,
    // even when assigning the selected quality does not change the setting.
    public void OnVRGraphicsQualityValueChanged(object sender, RoutedEventArgs args)
    {
        var quality = (VirtualDesktop.Mobile.VRGraphicsQuality)(int)Math.Round(((Slider)sender).Value);
        VirtualDesktop.Mobile.SharedUserSettings.Default.VRGraphicsQuality = quality;
        if (quality == VirtualDesktop.Mobile.VRGraphicsQuality.Potato || (int)quality >= 3)
        {
            QualitySounds++;
            LastSound = quality.ToString();
        }
        UpdateRestartQualityColor();
    }
    public void OnVRGraphicsQualityChanged(object sender, EventArgs args)
    {
        if (ThrowQualityRefresh) throw new InvalidOperationException("quality refresh fixture failure");
        _sldQuality.Value = (int)VirtualDesktop.Mobile.SharedUserSettings.Default.VRGraphicsQuality;
        QualityLabel = VirtualDesktop.Mobile.SharedUserSettings.Default.VRGraphicsQuality.ToString();
        QualityRefreshes++;
    }
    public void UpdateRestartQualityColor() { RestartQualityRefreshes++; }
    public void UpdateVRBitrate()
    {
        Displayed = Xenko.VR.HmdResolutionTypeExtensions.Rate(_sldBitrate.Limit == 0 ? _sldBitrate.Value : Math.Min(_sldBitrate.Value, _sldBitrate.Limit), true);
    }
}
public class LimitsSettingsTab
{
    public LimitSlider _sldBitrate = new LimitSlider { Value = 0.17f };
    public int Displayed;
    public void OnDesktopBitrateValueChanged(object sender, object args)
    {
        Displayed = Xenko.VR.HmdResolutionTypeExtensions.Rate(_sldBitrate.Limit == 0 ? _sldBitrate.Value : Math.Min(_sldBitrate.Value, _sldBitrate.Limit), false);
    }
}

internal static class StreamingLimitsChecks
{
    private const BindingFlags Private = BindingFlags.NonPublic | BindingFlags.Static;
    private static readonly Type Limits = typeof(StreamingLimits);
    private static VirtualDesktop.Mobile.SharedUserSettings User => VirtualDesktop.Mobile.SharedUserSettings.Default;
    private static VirtualDesktop.Mobile.SharedMobileSettings Mobile => VirtualDesktop.Mobile.SharedMobileSettings.Default;
    private static VirtualDesktop.Mobile.SharedStreamerSettings Streamer => VirtualDesktop.Mobile.SharedStreamerSettings.Default;
    private static string PathName => (string)Limits.GetMethod("FlagPath", Private).Invoke(null, null);
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    private static void Near(float value, float expected) { Check(Math.Abs(value - expected) < 0.0001f, value + " != " + expected); }
    private static T Add<T>(UIElement parent, T child) where T : UIElement
    {
        parent.Children.Add(child); parent.VisualChildren.Add(child); child.VisualParent = parent; return child;
    }
    private static MockPanel Panel()
    {
        var panel = new MockPanel();
        for (int i = 0; i < 7; i++) panel._tabPanels[i] = new object();
        var vr = new LimitsStreamingTab();
        var desktop = new LimitsSettingsTab();
        Add(panel._gridMain, vr._sldQuality);
        Add(panel._gridMain, vr._sldBitrate);
        Add(panel._gridMain, desktop._sldBitrate);
        panel._tabPanels[4] = desktop;
        panel._tabPanels[5] = vr;
        User.VRGraphicsQualityChanged += vr.OnVRGraphicsQualityChanged;
        User.VRBitrateLimitChanged += (s, e) => vr._sldBitrate.Value = User.VRBitrateLimit;
        User.DesktopBitrateLimitChanged += (s, e) => desktop._sldBitrate.Value = User.DesktopBitrateLimit;
        return panel;
    }
    private static void Tick(MockGame game) { game.Invoke(() => StreamingLimits.Tick(game)); }
    private static MockOverlay Overlay()
    {
        return new MockOverlay {
            _tbMaxBitrate = new TextBlock { Text = "400 Mbps", Font = "latin-font" },
            _tbBitrate = new TextBlock { Text = "1000 Mbps", Font = "latin-font" }
        };
    }
    private static void OverlayCheck(string scenario, MockGame game)
    {
        MockOverlay overlay = null;
        game.Invoke(() => game.PerformanceOverlay = overlay = Overlay());
        var label = (TextBlock)overlay._tbMaxBitrate;
        var current = (TextBlock)overlay._tbBitrate;
        int userWrites = User.Writes, currentWrites = current.TextWrites;
        Mobile.MeasuredBandwidth = 1000000000;
        game.Drain(); Tick(game);
        Check(label.Text == "1000 Mbps", "overlay maximum still uses the stock 400 Mbps ceiling");
        Check(current.Text == "1000 Mbps" && current.TextWrites == currentWrites && current.Listeners == 0,
            "current bitrate was replaced with a range ceiling or slider value");
        Check((string)label.Font == "latin-font" && label.TextSize == 20 &&
            (string)label.TextColor == "original-color", "overlay appearance changed");
        if (scenario == "overlay")
        {
            Mobile.MeasuredBandwidth = 1500000000; game.Drain();
            Check(label.Text == "1200 Mbps", "overlay maximum exceeds the extended slider ceiling");
            int writes = label.TextWrites, posts = game.Posts;
            overlay._forceDrawAll = false;
            for (int i = 0; i < 10000; i++) Tick(game);
            Check(label.TextWrites == writes && game.Posts == posts && !overlay._forceDrawAll,
                "idle overlay ticks write text, queue work or redraw");
        }
        else if (scenario == "overlay-native")
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR");
            int posts = game.Posts;
            game.Invoke(() => {
                label.Text = "400 Mbps"; // VD opening/reopening the overlay.
                Check(label.Text == "1000 Mbps", "stock maximum can render before correction");
            });
            Check(game.Posts == posts, "native label correction was deferred");
            Check(overlay._forceDrawAll, "changed maximum did not redraw its own overlay");
        }
        else if (scenario == "overlay-events")
        {
            Streamer.ActiveCodec = 2; game.Drain();
            Check(label.Text == "450 Mbps", "overlay ignored codec-specific maximum");
            Mobile.MeasuredBandwidth = 80000000; game.Drain();
            Check(label.Text == "80 Mbps", "overlay ignored measured bandwidth below stock maximum");
            Mobile.MeasuredBandwidth = 5000000; game.Drain();
            Check(label.Text == "5.0 Mbps", "low-bandwidth units or precision changed");
            Mobile.MeasuredBandwidth = 0; game.Drain();
            Check(label.Text == "5.0 Mbps" && Mobile.MeasuredBandwidth == 0, "zero measurement was fabricated");
            Streamer.ActiveCodec = 5; Mobile.MeasuredBandwidth = 1000000000; game.Drain();
            Mobile.FoveatedStreaming = true; Mobile.FoveationChanged(); game.Drain();
            Check(label.Text == "600 Mbps", "overlay ignored foveation-specific maximum");
        }
        else if (scenario == "overlay-reload")
        {
            MockOverlay next = null;
            game.Invoke(() => game.PerformanceOverlay = next = Overlay()); Tick(game);
            var replacement = (TextBlock)next._tbMaxBitrate;
            Check(label.Listeners == 0 && replacement.Listeners == 1 && replacement.Text == "1000 Mbps",
                "overlay recreation leaked or failed to rebind the maximum listener");
            game.Invoke(() => label.Text = "old overlay");
            Check(label.Text == "old overlay", "detached label still has an active callback");
            Mobile.MeasuredBandwidth = 800000000; // Leave one old-root refresh queued.
            game.Disposing(); game.Drain();
            Check(replacement.Listeners == 0, "disposed overlay retained its listener");
            game.Invoke(() => replacement.Text = "disposed");
            Tick(game);
            Check(replacement.Text == "disposed", "disposed root rebound its overlay");
            game.Invoke(() => { game.ControlPanel = Panel(); game.PerformanceOverlay = Overlay(); });
            Tick(game); game.Drain();
            Check(((TextBlock)((MockOverlay)game.PerformanceOverlay)._tbMaxBitrate).Text == "800 Mbps",
                "device recreation did not refresh overlay maximum");
        }
        else if (scenario == "overlay-late")
        {
            game.PerformanceOverlay = null; Tick(game);
            Check(label.Listeners == 0, "absent overlay retained listener");
            game.PerformanceOverlay = new MockOverlay(); Tick(game);
            game.Invoke(() => ((MockOverlay)game.PerformanceOverlay)._tbMaxBitrate = new TextBlock { Text = "400 Mbps" });
            Tick(game);
            Check(((TextBlock)((MockOverlay)game.PerformanceOverlay)._tbMaxBitrate).Text == "1000 Mbps",
                "late-loaded maximum was not bound");
        }
        Check(Mobile.MeasuredBandwidth == (scenario == "overlay" ? 1500000000 :
            scenario == "overlay-reload" ? 800000000 : 1000000000), "overlay display changed real bandwidth");
        Check(User.Writes == userWrites && !File.Exists(PathName), "overlay display changed user settings or disk");
    }
    internal static void Run(string scenario)
    {
        if (File.Exists(PathName)) File.Delete(PathName);
        bool soundRestore = scenario.StartsWith("sound-resume-") || scenario == "sound-reload" || scenario == "sound-refresh";
        var restoredQuality = VirtualDesktop.Mobile.VRGraphicsQuality.Rampage;
        if (scenario.StartsWith("sound-resume-"))
            restoredQuality = (VirtualDesktop.Mobile.VRGraphicsQuality)Enum.Parse(typeof(VirtualDesktop.Mobile.VRGraphicsQuality), scenario.Substring("sound-resume-".Length));
        if (soundRestore) File.WriteAllText(PathName, "vr=2.5\ndesktop=2\nquality=" + (int)restoredQuality + "\n");
        if (scenario == "resume") File.WriteAllText(PathName, "bitrate=0\nresolution=0\nvr=2.5\ndesktop=2\nquality=7\n");
        if (scenario == "invalid") File.WriteAllText(PathName, "bitrate=garbage\nresolution=garbage\nvr=NaN\ndesktop=Infinity\nquality=bad\n");
        var panel = Panel();
        var game = new MockGame { ControlPanel = panel };
        var vr = (LimitsStreamingTab)panel._tabPanels[5];
        var desktop = (LimitsSettingsTab)panel._tabPanels[4];
        MockGame.RequireUiThread = true;
        Tick(game);
        Check(panel._gridMain.Children.Count == 3, "unlock added controls to the native slider layout");
        Near(vr._sldQuality.Maximum, 7);
        Near(vr._sldBitrate.Maximum, (1200000000f - 10000000) / 390000000);
        Near(desktop._sldBitrate.Maximum, (360000000f - 4000000) / 116000000);
        if (scenario.StartsWith("overlay")) { OverlayCheck(scenario, game); return; }
        if (soundRestore)
        {
            Check(User.VRGraphicsQuality == restoredQuality && vr._sldQuality.Value == (int)restoredQuality,
                "silent restore lost the saved quality");
            Check(vr.QualitySounds == 0, "programmatic restore played " + vr.LastSound);
            Check(vr.QualityLabel == restoredQuality.ToString() && vr.QualityRefreshes > 0 && vr.RestartQualityRefreshes > 0,
                "silent restore skipped native dependent UI or restart state");
            if (scenario == "sound-reload")
            {
                game.Disposing();
                game.Invoke(() => game.ControlPanel = Panel());
                Tick(game);
                vr = (LimitsStreamingTab)((MockPanel)game.ControlPanel)._tabPanels[5];
                Check(vr.QualitySounds == 0 && vr._sldQuality.Value == 6 && vr.QualityLabel == "Rampage",
                    "recreated UI replayed the tier sound or lost the selected quality");
            }
            if (scenario == "sound-refresh")
            {
                Mobile.MeasuredBandwidth = 700000000;
                Streamer.ActiveCodec = 2;
                Mobile.FoveationChanged();
                game.Drain();
                for (int i = 0; i < 100; i++) Tick(game);
                Check(vr.QualitySounds == 0 && User.VRGraphicsQuality == restoredQuality,
                    "bandwidth/codec refresh or an idle tick replayed the tier sound");
            }
            // The next real slider event must still use the original handler.
            var nextQuality = restoredQuality == VirtualDesktop.Mobile.VRGraphicsQuality.Monster ?
                VirtualDesktop.Mobile.VRGraphicsQuality.Rampage : VirtualDesktop.Mobile.VRGraphicsQuality.Monster;
            game.Invoke(() => vr._sldQuality.Value = (int)nextQuality);
            Check(vr.QualitySounds == 1 && vr.LastSound == nextQuality.ToString() && User.VRGraphicsQuality == nextQuality,
                "restore removed the native manual-change sound or settings synchronization");
            Tick(game);
            Check(File.ReadAllText(PathName).Contains("quality=" + (int)nextQuality), "manual quality no longer persists");
            return;
        }
        if (scenario == "sound-finally")
        {
            vr.ThrowQualityRefresh = true;
            bool failed = false;
            // Force a real synchronization instead of the unchanged fast path.
            game.Invoke(() => {
                try { Limits.GetMethod("SetQuality", Private).Invoke(null, new object[] { 6f }); }
                catch (TargetInvocationException) { failed = true; }
            });
            vr.ThrowQualityRefresh = false;
            Check(failed, "fixture did not exercise a failed quality refresh");
            game.Invoke(() => vr._sldQuality.Value = 5);
            Check(vr.QualitySounds == 1 && vr.LastSound == "Monster", "failed restore did not reattach native sound handler");
            return;
        }
        if (scenario == "default" || scenario == "invalid")
        {
            Check(User.Writes == 0, "default install changed stock selections");
            if (scenario == "default") Check(!File.Exists(PathName), "display created a preference file");
            int count = panel._gridMain.Children.Count;
            for (int i = 0; i < 100; i++) Tick(game);
            Check(count == panel._gridMain.Children.Count, "duplicate rows");
            panel._forceDrawAll = false; Tick(game);
            Check(!panel._forceDrawAll, "idle tick redraws panel");
            return;
        }
        if (scenario == "resume")
        {
            Near(User.VRBitrateLimit, 2.5f); Near(User.DesktopBitrateLimit, 2);
            Check((int)User.VRGraphicsQuality == 7 && vr._sldQuality.Value == 7, "extended quality lost on restart");
            return;
        }
        if (scenario == "bounds")
        {
            Near(User.VRBitrateLimit, 0.36f); Near(User.DesktopBitrateLimit, 0.17f);
            Check((int)User.VRGraphicsQuality == 1, "unlock raised selection by itself");
        }
        else if (scenario == "bandwidth" || scenario == "codec")
        {
            game.Invoke(() => User.VRBitrateLimit = 2);
            Mobile.MeasuredBandwidth = 700000000;
            game.Drain();
            Check(Mobile.MeasuredBandwidth == 700000000, "faked measured bandwidth");
            Check(vr.Displayed > 400000000 && vr.Displayed <= Mobile.MeasuredBandwidth, "display does not match target");
            if (scenario == "codec")
            {
                Streamer.ActiveCodec = 2; game.Drain();
                Near(vr._sldBitrate.Maximum, (450000000f - 10000000) / 140000000);
                Check(vr.Displayed == 290000000, "codec switch changed normalized semantics");
            }
            Mobile.MeasuredBandwidth = 80000000; game.Drain();
            Check(vr.Displayed <= 80500000, "real network cap was removed");
            Mobile.MeasuredBandwidth = 0; game.Drain();
            Near(vr._sldBitrate.Limit, 0);
        }
        else if (scenario == "lower")
        {
            game.Invoke(() => { User.VRBitrateLimit = 2; User.DesktopBitrateLimit = 2; User.VRGraphicsQuality = VirtualDesktop.Mobile.VRGraphicsQuality.Unstoppable; });
            game.Invoke(() => { User.VRBitrateLimit = 0.36f; User.DesktopBitrateLimit = 0.17f; User.VRGraphicsQuality = VirtualDesktop.Mobile.VRGraphicsQuality.Medium; });
            Tick(game);
            Near(User.VRBitrateLimit, 0.36f); Near(User.DesktopBitrateLimit, 0.17f);
            Check((int)User.VRGraphicsQuality == 1, "native lower selection was overridden");
            Near(vr._sldQuality.Maximum, 7);
        }
        else if (scenario == "save")
        {
            game.Invoke(() => { User.VRBitrateLimit = 2.3f; User.VRGraphicsQuality = VirtualDesktop.Mobile.VRGraphicsQuality.Monster; });
            Check(!File.Exists(PathName), "slider events wrote preferences on the hot path");
            Tick(game);
            string saved = File.ReadAllText(PathName);
            Check(saved.Contains("vr=2.3") && saved.Contains("quality=5"), "choice did not persist on the existing tick");
            game.Invoke(() => User.VRBitrateLimit = 2.4f);
            game.Disposing();
            Check(File.ReadAllText(PathName).Contains("vr=2.4"), "disposal did not flush pending selection");
        }
        else if (scenario == "reload")
        {
            Mobile.MeasuredBandwidth = 500000000;
            game.Disposing();
            Check(Limits.GetField("_root", Private).GetValue(null) == null, "disposed root retained");
            Tick(game);
            Check(panel._gridMain.Children.Count == 3, "rebound disposed panel");
            MockPanel next = null;
            game.Invoke(() => next = Panel()); game.ControlPanel = next;
            Tick(game); game.Drain();
            Near(((LimitsStreamingTab)next._tabPanels[5])._sldQuality.Maximum, 7);
            Check(panel._gridMain.Children.Count == 3 && next._gridMain.Children.Count == 3, "rebuild added controls");
        }
        else throw new Exception("unknown streaming test " + scenario);
    }
}
