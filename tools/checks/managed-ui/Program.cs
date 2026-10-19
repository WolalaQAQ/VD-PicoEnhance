using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;

// Host doubles for the production mod. Content is a UIElement (as in the
// shipped Xenko assembly), and BeginInvoke queues rather than running inline.
namespace Xenko.UI
{
    public class UIElement
    {
        public string Name { get; set; }
        public bool IsEnabled { get; set; } = true;
        public UIElement VisualParent { get; set; }
        private readonly List<UIElement> _children = new List<UIElement>();
        private readonly List<UIElement> _visualChildren = new List<UIElement>();
        private object _font;
        public int FontWrites;
        public int ChildReads, VisualReads;
        public List<UIElement> Children { get { ChildReads++; return _children; } }
        public List<UIElement> VisualChildren { get { VisualReads++; return _visualChildren; } }
        public bool IsDirty { get; set; }
        public object BackgroundColor { get; set; }
        public float TextSize { get; set; } = 20;
        public object TextColor { get; set; } = "original-color";
        public object Font
        {
            get { return _font; }
            set { MockGame.AssertUiWrite(); FontWrites++; _font = value; }
        }
    }
}
namespace Xenko.UI.Controls
{
    public class ContentControl : Xenko.UI.UIElement { public Xenko.UI.UIElement Content { get; set; } }
    public class CheckBox : ContentControl { public bool IsChecked { get; set; } }
    public class TextBlock : Xenko.UI.UIElement
    {
        private string _text;
        private EventHandler _changed;
        public int TextWrites, Listeners;
        public event EventHandler TextChanged
        {
            add { _changed += value; Listeners++; }
            remove { _changed -= value; Listeners--; }
        }
        public string Text
        {
            get { return _text; }
            set
            {
                MockGame.AssertUiWrite();
                if (_text == value) return;
                TextWrites++;
                _text = value;
                _changed?.Invoke(this, EventArgs.Empty);
            }
        }
    }
    public class EditText : TextBlock { }
}
namespace Xenko.Engine
{
    public class UILibrary { public Dictionary<string, Xenko.UI.UIElement> UIElements { get; } = new Dictionary<string, Xenko.UI.UIElement>(); }
    public class UIPage { public Xenko.UI.UIElement RootElement { get; set; } }
}
namespace VirtualDesktop.Mobile { public static class InputManager { public static object _game; } }
namespace VdHsMod
{
    internal static class ModLog
    {
        internal static void Info(string value) { }
        internal static void Error(string value) { throw new Exception(value); }
    }
}
public class MockGame
{
    public static bool RequireUiThread, OnUiThread;
    public int Dispatches, Posts;
    public object ControlPanel { get; set; }
    public object PerformanceOverlay { get; set; }
    private readonly Queue<Action> _queue = new Queue<Action>();
    public event EventHandler<EventArgs> DeviceCreated, DeviceDisposing;
    public static void AssertUiWrite()
    {
        if (RequireUiThread && !OnUiThread) throw new InvalidOperationException("UI write outside dispatcher");
    }
    public bool CheckAccess() { return OnUiThread; }
    public void Invoke(Action action)
    {
        Dispatches++;
        bool previous = OnUiThread;
        OnUiThread = true;
        try { action(); }
        finally { OnUiThread = previous; }
    }
    public void BeginInvoke(Action action) { Posts++; _queue.Enqueue(action); }
    public void Drain()
    {
        int count = 0;
        while (_queue.Count > 0)
        {
            if (++count > 1000) throw new Exception("recursive text-change queue");
            Invoke(_queue.Dequeue());
        }
    }
    public void Created() { DeviceCreated?.Invoke(this, EventArgs.Empty); }
    public void Disposing() { DeviceDisposing?.Invoke(this, EventArgs.Empty); }
}
public class NoDispatcherGame { public object ControlPanel { get; set; } }
public class MockUiSystem
{
    protected MockContentManager Content { get; private set; }
    public void SetContent(MockContentManager value) { Content = value; }
}
public class MockContentManager
{
    public object Page;
    public string Url = "UI/ControlPanel/Page";
    public int Queries, Loads;
    public object Get(Type type, string url)
    {
        Queries++;
        if (type != typeof(Xenko.Engine.UIPage) || url != Url) throw new Exception("wrong cached resource query");
        return Page;
    }
    public object Load(Type type, string url) { Loads++; throw new Exception("must not load resource copies"); }
}
public class MockPanel : MockUiSystem
{
    public object[] _tabPanels = new object[7];
    public Xenko.UI.UIElement _gridMain = new Xenko.UI.UIElement();
    public object _topPanel, _bottomPanel, _currentVisibleTab;
    public bool _forceDrawAll;
    public object Library { get; set; } = new Xenko.Engine.UILibrary();
}
public class MockOverlay : MockUiSystem
{
    public Xenko.UI.UIElement _grid = new Xenko.UI.UIElement();
    public bool _forceDrawAll;
    public Xenko.UI.UIElement _tbFramerate, _tbLatency, _tbBitrate, _tbMaxBitrate, _tbWifi,
        _tbGame, _tbEncoding, _tbNetworking, _tbDecoding, _tbResolution, _tbTargetFramerate,
        _tbGPU, _tbCodec, _tbRuntime;
}
public class MockInputTab { public Xenko.UI.Controls.CheckBox _chkAutoHideHands; }
public class MockComputersTab { public Xenko.UI.UIElement _pnComputers, _tbSSID, _tbWifiInfo, _tbAccountID, _tbMessage; }
public class MockGamesTab { public Xenko.UI.UIElement _gridOculusGames, _gridSteamGames; }
public class MockVideosTab { public Xenko.UI.UIElement _gridFiles, _pnFolders, _tbMessage; }
public class MockTopPanel { public Xenko.UI.UIElement _tbTime, _tbHeadsetBattery; }
public class MockBottomPanel { public Xenko.UI.UIElement _tbFrameRate, _tbVideoFrameRate, _tbVideoBitrate, _tbAssistant; }

public static class Program
{
    private const BindingFlags PrivateStatic = BindingFlags.NonPublic | BindingFlags.Static;
    private static readonly Type Toggle = typeof(VdHsMod.GestureToggle), Localize = typeof(VdHsMod.UiLocalize);
    private static object Call(Type type, string method, params object[] args) { return type.GetMethod(method, PrivateStatic).Invoke(null, args); }
    private static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
    private static T Add<T>(Xenko.UI.UIElement parent, T child) where T : Xenko.UI.UIElement
    {
        parent.Children.Add(child);
        parent.VisualChildren.Add(child);
        child.VisualParent = parent;
        return child;
    }
    private static Xenko.UI.Controls.TextBlock Label(Xenko.UI.UIElement parent, string text, string name = null)
    {
        return Add(parent, new Xenko.UI.Controls.TextBlock { Name = name, Text = text, Font = "latin-font" });
    }
    private static MockPanel NewPanel()
    {
        var panel = new MockPanel();
        for (int i = 0; i < 7; i++) panel._tabPanels[i] = new object();
        var anchor = Add(panel._gridMain, new Xenko.UI.Controls.CheckBox());
        panel._tabPanels[3] = new MockInputTab { _chkAutoHideHands = anchor };
        panel._bottomPanel = new MockBottomPanel { _tbAssistant = Label(panel._gridMain, "寻求帮助") };
        ((MockBottomPanel)panel._bottomPanel)._tbAssistant.Font = "cjk-font";
        panel.SetContent(new MockContentManager { Page = new Xenko.Engine.UIPage { RootElement = panel._gridMain } });
        return panel;
    }
    private static MockPanel Panel()
    {
        var panel = NewPanel();
        VirtualDesktop.Mobile.InputManager._game = new MockGame { ControlPanel = panel, PerformanceOverlay = NewOverlay() };
        return panel;
    }
    private static MockOverlay NewOverlay()
    {
        var overlay = new MockOverlay();
        overlay.SetContent(new MockContentManager { Url = "UI/PerformanceOverlay/Page",
            Page = new Xenko.Engine.UIPage { RootElement = overlay._grid } });
        return overlay;
    }
    private static void Table() { Localize.GetField("_table", PrivateStatic).SetValue(null, Call(Localize, "LoadTable", "zh-CN")); }
    private static MockContentManager Content(MockUiSystem panel)
    {
        return (MockContentManager)typeof(MockUiSystem).GetProperty("Content", BindingFlags.Instance | BindingFlags.NonPublic).GetValue(panel);
    }
    private static void Rejected(Action action)
    {
        try { action(); }
        catch (TargetInvocationException ex)
        {
            Exception cause = ex;
            while (cause is TargetInvocationException && cause.InnerException != null) cause = cause.InnerException;
            Check(cause is InvalidOperationException, "wrong rejection: " + cause.Message);
            return;
        }
        throw new Exception("incompatible host was accepted");
    }
    private static void ToggleTest(string scenario)
    {
        var panel = Panel();
        string path = (string)Call(Toggle, "FlagPath");
        if (File.Exists(path)) File.Delete(path);
        // Old injection metadata is no longer a second gate for the UI.
        Environment.SetEnvironmentVariable("VDHS_SYSTEM_GESTURES_ENABLED", scenario == "legacy-disabled" ? "0" : null);
        string saved = scenario == "saved-off" ? "0\n" : scenario == "saved-on" ? "1\n" :
            scenario == "invalid" ? "invalid\n" : scenario == "spaced-off" ? "0 \n" :
            scenario == "spaced-on" ? " 1\n" : scenario == "crlf-off" ? "0\r\n" : null;
        if (saved != null) File.WriteAllText(path, saved);
        var anchor = ((MockInputTab)panel._tabPanels[3])._chkAutoHideHands;
        anchor.Content = Label(anchor, "Automatically hide controllers");
        if (scenario == "toggle-zh")
        {
            Table();
            Check((bool)Call(Localize, "Tick"), "Chinese initializer was not ready");
        }
        if (scenario == "toggle-en")
            Localize.GetField("_table", PrivateStatic).SetValue(null, Call(Localize, "LoadTable", "en-US"));
        int original = panel._gridMain.Children.Count;
        MockGame.RequireUiThread = true;
        Check((bool)Call(Toggle, "TryInstall"), "row was not installed");
        var box = (Xenko.UI.Controls.CheckBox)panel._gridMain.Children[original];
        Check(box.IsChecked == (scenario == "saved-on"), "incorrect initial state");
        Check(box.IsEnabled, "checkbox is not interactive");
        var text = (Xenko.UI.Controls.TextBlock)box.Content;
        Check(text.Text == (scenario == "toggle-zh" ? "屏蔽系统手势" : "Suppress system gestures"), "wrong switch language");
        Check((string)text.Font == (scenario == "toggle-zh" ? "cjk-font" : "latin-font"), "wrong switch font");
        Call(Toggle, "Mirror");
        Check(saved == null ? !File.Exists(path) : File.ReadAllText(path) == saved, "initial display rewrote user's setting");
        if (scenario == "roundtrip")
        {
            box.IsChecked = true;
            Call(Toggle, "Mirror");
            Check(File.ReadAllText(path).Trim() == "1", "ON was not saved");
            box.IsChecked = false;
            Call(Toggle, "Mirror");
            Check(File.ReadAllText(path).Trim() == "0", "OFF was not saved");
            Check(panel._gridMain.Children.Contains(box), "OFF removed the checkbox");
            box.IsChecked = true;
            Call(Toggle, "Mirror");
            Check(File.ReadAllText(path).Trim() == "1", "ON again was not saved");
        }
        if (scenario == "toggle-reload")
        {
            box.IsChecked = true;
            Call(Toggle, "Mirror");
            var game = (MockGame)VirtualDesktop.Mobile.InputManager._game;
            game.Invoke(() => game.ControlPanel = NewPanel());
            Call(Toggle, "Mirror");
            var next = (MockPanel)game.ControlPanel;
            var replacement = (Xenko.UI.Controls.CheckBox)next._gridMain.Children[next._gridMain.Children.Count - 1];
            Check(replacement.IsChecked && replacement.IsEnabled, "recreated row lost saved state");
        }
    }
    private static void LocalizationTest()
    {
        var panel = Panel();
        var root = panel._gridMain;
        var game = (MockGame)VirtualDesktop.Mobile.InputManager._game;
        var captions = new List<Xenko.UI.Controls.TextBlock>();
        foreach (string source in new[] { "Computers", "Environment", "Oculus Games", "Input Options", "Auto connect", "VR Bitrate", "Paste URL" })
            captions.Add(Label(Add(root, new Xenko.UI.UIElement()), source));
        var brand = Label(root, "Snapdragon® Game Super Resolution");
        var technology = Label(root, "Synchronous Spacewarp (SSW)");
        var computers = new MockComputersTab
        {
            _pnComputers = Add(root, new Xenko.UI.UIElement()),
            _tbSSID = Label(root, "High"), _tbAccountID = Label(root, "Name"), _tbMessage = Label(root, "Low")
        };
        panel._tabPanels[0] = computers;
        var computerName = Label(computers._pnComputers, "Low", "tbName");
        // Real computer cards have their fixed disconnect button at index 2.
        computers._pnComputers.Children.Remove(computerName);
        computers._pnComputers.VisualChildren.Remove(computerName);
        var card = Add(computers._pnComputers, new Xenko.UI.UIElement());
        Add(card, computerName);
        Label(card, "Name", "tbDescription");
        var disconnect = Label(card, "Disconnect");
        var warning = Label(Add(card, new Xenko.UI.UIElement()), "Not on same local network");
        var computerStatus = Label(card, "Connected", "tbStatus");
        // Dynamic subtrees may also be referenced by Content aliases.
        Add(root, new Xenko.UI.Controls.ContentControl { Content = computers._pnComputers });
        var games = new MockGamesTab { _gridSteamGames = Add(root, new Xenko.UI.UIElement()) };
        panel._tabPanels[2] = games;
        var gameName = Label(games._gridSteamGames, "High");
        var videos = new MockVideosTab { _gridFiles = Add(root, new Xenko.UI.UIElement()), _pnFolders = Add(root, new Xenko.UI.UIElement()) };
        panel._tabPanels[6] = videos;
        var file = Label(videos._gridFiles, "Low");
        var folder = Label(videos._pnFolders, "Name");
        var bottom = (MockBottomPanel)panel._bottomPanel;
        bottom._tbFrameRate = Label(root, "60 fps");
        var editor = Add(root, new Xenko.UI.Controls.EditText { Text = "Name" });
        var editorChild = Label(editor, "Low");
        var nestedText = new Xenko.UI.Controls.TextBlock { Text = "High" };
        Add(root, new Xenko.UI.Controls.ContentControl { Content = nestedText });
        var tab = Label(root, "INPUT");
        tab.BackgroundColor = "original-bg";
        Table();
        MockGame.RequireUiThread = true;
        Check((bool)Call(Localize, "Tick"), "resource initialization did not finish");
        game.Drain();
        foreach (var caption in captions) Check(caption.Text != "" && (string)caption.Font == "cjk-font", "a page was not localized");
        Check(captions[0].Text == "电脑" && captions[6].Text == "粘贴链接", "page captions not translated");
        Check(brand.Text == "Snapdragon® Game Super Resolution" && (string)brand.Font == "latin-font",
            "brand caption or its original font changed");
        Check(technology.Text == "Synchronous Spacewarp (SSW)" && (string)technology.Font == "latin-font",
            "technology caption or its original font changed");
        foreach (var item in new[] { computerName, gameName, file, folder, (Xenko.UI.Controls.TextBlock)computers._tbSSID,
            (Xenko.UI.Controls.TextBlock)computers._tbAccountID, (Xenko.UI.Controls.TextBlock)computers._tbMessage,
            (Xenko.UI.Controls.TextBlock)bottom._tbFrameRate, editorChild, computerStatus })
            Check(item.Listeners == 0 && (string)item.Font == "latin-font", "dynamic data acquired a listener/font");
        Check(computerName.Text == "Low" && gameName.Text == "High" && file.Text == "Low" && folder.Text == "Name", "user names changed");
        Check(disconnect.Text == "断开连接" && disconnect.Listeners == 1, "existing computer card missed its fixed button");
        Check(warning.Text == "不在同一局域网内" && warning.Listeners == 1, "existing computer card missed its fixed warning");
        Check(editor.Text == "Name" && editor.Listeners == 0, "editable input changed");
        Check(nestedText.Text == "高", "Content TextBlock did not translate");
        Check(tab.Text == "输入" && (string)tab.BackgroundColor == "original-bg", "padding/background masking");
        Check(panel._forceDrawAll && root.IsDirty, "normal full redraw was not requested");
        Check(Content(panel).Loads == 0 && Content(panel).Queries == 1, "resource was reloaded");
    }
    private static void LifecycleTest(string scenario)
    {
        var panel = Panel();
        var game = (MockGame)VirtualDesktop.Mobile.InputManager._game;
        var root = panel._gridMain;
        var label = Label(root, "Low");
        var help = ((MockBottomPanel)panel._bottomPanel)._tbAssistant;
        if (scenario == "font-late") help.Font = null;
        if (scenario == "resource-late") Content(panel).Page = null;
        if (scenario == "incomplete-page") panel._tabPanels[6] = null;
        Table();
        MockGame.RequireUiThread = true;
        if (scenario == "no-dispatcher")
        {
            VirtualDesktop.Mobile.InputManager._game = new NoDispatcherGame { ControlPanel = panel };
            Rejected(() => Call(Localize, "Tick"));
            Check(label.Text == "Low" && label.Listeners == 0, "off-thread mutation");
            return;
        }
        if (scenario == "resource-alias")
        {
            ((Xenko.Engine.UIPage)Content(panel).Page).RootElement = new Xenko.UI.UIElement();
            Rejected(() => Call(Localize, "Tick"));
            Check(label.Text == "Low" && label.Listeners == 0, "mutation before alias check");
            return;
        }
        if (scenario == "font-late" || scenario == "resource-late" || scenario == "incomplete-page")
        {
            Check(!(bool)Call(Localize, "Tick"), "accepted incomplete page/font");
            Check(label.Text == "Low" && label.Listeners == 0, "mutated before readiness");
            game.Invoke(() =>
            {
                help.Font = "cjk-font";
                Content(panel).Page = new Xenko.Engine.UIPage { RootElement = root };
                if (panel._tabPanels[6] == null) panel._tabPanels[6] = new object();
            });
        }
        Check((bool)Call(Localize, "Tick"), "initialization failed");
        Check(label.Text == "低" && label.Listeners == 1, "initial caption/listener missing");
        int reads = root.ChildReads, dispatches = game.Dispatches, writes = label.TextWrites;
        panel._forceDrawAll = false;
        root.IsDirty = false;
        Call(Localize, "Tick");
        Check(root.ChildReads == reads && game.Dispatches == dispatches && label.TextWrites == writes && !panel._forceDrawAll,
            "completed initializer still scans/dispatches/writes");
        if (scenario == "cache") return;
        if (scenario == "repeat-caption" || scenario == "repeat-async" || scenario == "repeat-font")
        {
            int fontWrites = label.FontWrites;
            if (scenario == "repeat-caption")
            {
                game.Invoke(() =>
                {
                    for (int i = 0; i < 1000; i++) label.Text = "Low";
                });
                Check(!panel._forceDrawAll && !root.IsDirty && label.FontWrites == fontWrites && game.Posts == 0,
                    "identical UI caption requested redraw, font invalidation or a later frame");
            }
            else if (scenario == "repeat-async")
            {
                MockGame.RequireUiThread = false;
                label.Text = "Low";
                MockGame.RequireUiThread = true;
                Check(label.Text == "Low" && game.Posts == 1, "async source did not wait for UI dispatcher");
                game.Drain();
                Check(panel._forceDrawAll && root.IsDirty && label.FontWrites == fontWrites,
                    "async repeated caption did not clear potentially rendered English");
            }
            else
            {
                game.Invoke(() =>
                {
                    label.Font = "changed-host-font";
                    label.Text = "Low";
                });
                Check(panel._forceDrawAll && root.IsDirty && (string)label.Font == "cjk-font",
                    "identical caption ignored a real font change");
            }
            Check(label.Text == "低" && root.ChildReads == reads, "repeated caption was not restored or rescanned the page");
        }
        else if (scenario == "quality-cycle")
        {
            string[] sources = { "Potato", "Low", "Medium", "High", "Ultra", "Godlike", "Low" };
            string[] expected = { "Potato", "低", "中", "高", "超高", "Godlike", "低" };
            for (int i = 0; i < sources.Length; i++)
            {
                int index = i;
                game.Invoke(() =>
                {
                    label.Text = sources[index];
                    Check(label.Text == expected[index], "quality change exposed English after setter returned");
                });
            }
            Check(game.Posts == 0 && label.Listeners == 1 && root.ChildReads == reads,
                "quality cycling queued work, duplicated events or scanned the page");
        }
        else if (scenario == "latest-caption")
        {
            MockGame.RequireUiThread = false;
            label.Text = "Medium";
            label.Text = "High";
            MockGame.RequireUiThread = true;
            game.Invoke(() => label.Text = "Low");
            Check(label.Text == "低", "UI event did not complete synchronously");
            game.Drain();
            Check(label.Text == "低", "pending async work overwrote a newer UI selection");
            Check(panel._forceDrawAll && root.IsDirty,
                "pending async callback lost the redraw needed to clear potentially rendered English");
        }
        else if (scenario == "new-toggle")
        {
            Call(Toggle, "TryInstall");
            Check(root.ChildReads == reads + 1, "late toggle rescanned root");
            var checkbox = (Xenko.UI.Controls.CheckBox)root.Children[root.Children.Count - 1];
            Check(((Xenko.UI.Controls.TextBlock)checkbox.Content).Text == "屏蔽系统手势", "late toggle not localized immediately");
        }
        else if (scenario == "new-root" || scenario == "pending-dispose")
        {
            if (scenario == "pending-dispose")
            {
                MockGame.RequireUiThread = false;
                label.Text = "High";
                MockGame.RequireUiThread = true;
            }
            MockPanel replacement = null;
            Xenko.UI.Controls.TextBlock next = null;
            game.Invoke(() =>
            {
                game.Disposing();
                replacement = NewPanel();
                next = Label(replacement._gridMain, "Quit");
                ((MockBottomPanel)replacement._bottomPanel)._tbAssistant.Font = "new-font";
                game.ControlPanel = replacement;
                game.PerformanceOverlay = NewOverlay();
                game.Created();
                Check(next.Text == "Quit", "created handler ran before LoadContent completed");
            });
            game.Drain();
            Check(label.Listeners == 0 && next.Text == "退出" && next.Listeners == 1 && (string)next.Font == "new-font", "reload did not release/rebind");
            if (scenario == "pending-dispose") Check(label.Text == "High", "queued callback touched disposed label");
        }
        else if (scenario == "event" || scenario == "new-tab" || scenario == "off-thread" || scenario == "unknown-text")
        {
            string source = scenario == "unknown-text" ? "not in dictionary" : "High";
            if (scenario == "off-thread")
            {
                MockGame.RequireUiThread = false; // VD's own async writer, not the mod.
                label.Text = source;
                MockGame.RequireUiThread = true;
            }
            else game.Invoke(() => label.Text = source);
            Check(label.Text == (scenario == "off-thread" || scenario == "unknown-text" ? source : "高"),
                "UI-originated caption was not localized before its setter returned");
            game.Drain();
            Check(label.Text == (scenario == "unknown-text" ? source : "高"), "event did not use latest caption");
            Check(root.ChildReads == reads, "event rescanned page");
            Check(panel._forceDrawAll, "caption/font change did not request redraw");
            Check(game.Posts == (scenario == "off-thread" ? 1 : 0) && label.Listeners == 1,
                "UI event queued a later frame or recursively subscribed");
            if (scenario == "unknown-text")
            {
                Check((string)label.Font == "latin-font", "unknown text retained the translation font");
                panel._forceDrawAll = false;
                game.Invoke(() => label.Text = "another unknown caption");
                Check(!panel._forceDrawAll, "unknown text without a font change requested extra redraw");
            }
        }
    }
    private static Xenko.UI.Controls.TextBlock CopyLabel(Xenko.UI.Controls.TextBlock source)
    {
        // Host-side copy only; this is not a test of Xenko's UICloner runtime.
        return new Xenko.UI.Controls.TextBlock { Text = source.Text, Font = source.Font, Name = source.Name };
    }
    private static void TemplateTest()
    {
        var panel = Panel();
        var library = (Xenko.Engine.UILibrary)panel.Library;
        var computer = new Xenko.UI.UIElement();
        var name = Label(computer, "Low", "tbName");
        var description = Label(computer, "Name", "tbDescription");
        var status = Label(computer, "Connected", "tbStatus");
        var button = Label(computer, "Disconnect");
        library.UIElements.Add("itemComputer", computer);
        var folder = new Xenko.UI.UIElement();
        var folderName = Label(folder, "Low");
        library.UIElements.Add("itemFolderButton", folder);
        var video = new Xenko.UI.UIElement();
        for (int i = 0; i < 5; i++) Add(video, new Xenko.UI.UIElement());
        Add(video.Children[3], new Xenko.UI.UIElement());
        var progress = Label(video.Children[3], "60 fps");
        var fileName = Label(video.Children[4], "Name");
        var local = Label(video.Children[4], "Local file");
        library.UIElements.Add("itemVideoFile", video);
        Table();
        MockGame.RequireUiThread = true;
        Call(Localize, "Tick");
        Check(button.Text == "断开连接" && local.Text == "本地文件", "template captions not translated");
        foreach (var label in new[] { name, description, status, folderName, progress, fileName })
            Check((string)label.Font == "latin-font" && label.Listeners == 0 && label.TextWrites == 1, "template user data changed");
        Check(button.Listeners == 0 && local.Listeners == 0, "template received event handlers");
        var game = (MockGame)VirtualDesktop.Mobile.InputManager._game;
        game.Invoke(() =>
        {
            var clone = CopyLabel(button);
            Check(clone.Text == "断开连接" && (string)clone.Font == "cjk-font" && clone.Listeners == 0, "copied template lost caption/font");
        });
    }
    private static void TableTest()
    {
        var table = (Dictionary<string, string>)Call(Localize, "LoadTable", "zh-CN");
        string source = "Your controllers won't appear in your VR games.\nAre you sure you want to do this?";
        Check(table[source].Contains("\n") && !table[source].Contains("\\n"), "escaped line break not decoded");
    }
    private static void LanguageTest()
    {
        foreach (string language in new[] { "zh", "zh-CN", "zh-TW", "zh-HK", "zh-Hans-CN", "zh-Hant-TW", "ZH-cn" })
            Check(Call(Localize, "LoadTable", language) != null, "Chinese system language was not enabled: " + language);
        foreach (string language in new[] { null, "", "en-US", "ja-JP", "de-DE", "ko-KR", "zho", "zhong" })
            Check(Call(Localize, "LoadTable", language) == null, "non-Chinese language loaded a dictionary: " + language);
    }
    private static void OverlayTest(string scenario)
    {
        var panel = Panel();
        var game = (MockGame)VirtualDesktop.Mobile.InputManager._game;
        var overlay = (MockOverlay)game.PerformanceOverlay;
        var root = overlay._grid;
        var title = Label(root, "Framerate");
        var headings = new List<Xenko.UI.Controls.TextBlock>();
        foreach (string source in new[] { "Latency", "Bitrate", "Max bitrate", "Codec", "Runtime", "Headset",
            "Game", "Encoding", "Networking", "Decoding", "Gfx Quality", "Render resolution", "Target framerate",
            "Video buffering", "Automatic bitrate" }) headings.Add(Label(root, source));
        var spacewarp = Label(root, "Spacewarp");
        var units = new List<Xenko.UI.Controls.TextBlock>();
        foreach (string unit in new[] { "fps", "ms", "%", "Mbps", "GHz" }) units.Add(Label(root, unit));
        var quality = Label(root, "High'");
        var status = Label(root, "No");
        var ethernet = Label(root, "PC Ethernet");
        var numeric = new List<Xenko.UI.Controls.TextBlock>();
        foreach (var field in typeof(MockOverlay).GetFields())
        {
            if (!field.Name.StartsWith("_tb", StringComparison.Ordinal)) continue;
            var label = Label(root, "High");
            field.SetValue(overlay, label);
            numeric.Add(label);
        }
        numeric.Add(Label(root, "Name", "tbHeadset"));
        numeric.Add(Label(root, "Yes", "tbVersion"));
        Table();
        MockGame.RequireUiThread = true;
        if (scenario == "overlay-late" || scenario == "overlay-page-late")
        {
            if (scenario == "overlay-late") overlay._grid = null;
            else Content(overlay).Page = null;
            Check(!(bool)Call(Localize, "Tick"), "finished before overlay resource was ready");
            Check(title.Text == "Framerate" && title.Listeners == 0, "mutated an incomplete overlay");
            overlay._grid = root;
            Content(overlay).Page = new Xenko.Engine.UIPage { RootElement = root };
        }
        if (scenario == "overlay-alias")
        {
            ((Xenko.Engine.UIPage)Content(overlay).Page).RootElement = new Xenko.UI.UIElement();
            Rejected(() => Call(Localize, "Tick"));
            Check(title.Text == "Framerate" && title.Listeners == 0, "mutated unshared overlay");
            return;
        }
        Check((bool)Call(Localize, "Tick"), "overlay initialization did not finish");
        Check(title.Text == "帧率" && quality.Text == "高'" && status.Text == "否" && ethernet.Text == "电脑有线网络",
            "independent overlay fixed captions or statuses were not translated");
        Check(overlay._forceDrawAll && root.IsDirty, "overlay did not request its own full redraw");
        Check(title.TextSize == 20 && (string)title.TextColor == "original-color", "localization changed size or colors");
        foreach (var heading in headings)
            Check((string)heading.Font == "cjk-font" && heading.TextSize == 20 && (string)heading.TextColor == "original-color",
                "a resource-verified performance heading was missed or restyled");
        Check(spacewarp.Text == "Spacewarp" && (string)spacewarp.Font == "latin-font", "Spacewarp name or font changed");
        foreach (var unit in units)
            Check(unit.TextWrites == 1 && (string)unit.Font == "latin-font", "overlay unit was translated or restyled");
        foreach (var label in numeric)
            Check(label.Listeners == 0 && label.TextWrites == 1 && (string)label.Font == "latin-font" && label.TextSize == 20,
                "overlay telemetry or metadata was localized");
        Check(Content(overlay).Loads == 0, "overlay resource was loaded again");
        int reads = root.ChildReads, queries = Content(overlay).Queries;
        overlay._forceDrawAll = panel._forceDrawAll = root.IsDirty = panel._gridMain.IsDirty = false;
        Call(Localize, "Tick");
        Check(root.ChildReads == reads && Content(overlay).Queries == queries && !overlay._forceDrawAll,
            "finished initializer still scanned the overlay");
        if (scenario == "overlay-reload" || scenario == "overlay-pending-dispose")
        {
            if (scenario == "overlay-pending-dispose")
            {
                MockGame.RequireUiThread = false;
                status.Text = "Active";
                MockGame.RequireUiThread = true;
            }
            MockOverlay replacement = null;
            Xenko.UI.Controls.TextBlock next = null;
            game.Invoke(() =>
            {
                game.Disposing();
                game.ControlPanel = NewPanel();
                replacement = NewOverlay();
                next = Label(replacement._grid, "No");
                game.PerformanceOverlay = replacement;
                game.Created();
            });
            game.Drain();
            Check(status.Listeners == 0 && next.Listeners == 1 && next.Text == "否" && replacement._forceDrawAll,
                "overlay lifecycle did not release old events and bind the replacement");
            if (scenario == "overlay-pending-dispose") Check(status.Text == "Active", "disposed overlay was touched by queued work");
            return;
        }
        game.Invoke(() =>
        {
            foreach (string source in new[] { "Yes", "Not Gigabit", "No", "2.0", "3.0" })
            {
                status.Text = source;
                string expected = source == "Yes" ? "是" : source == "Not Gigabit" ? "非千兆" : source == "No" ? "否" : source;
                Check(status.Text == expected, "PC connection state did not finish before render");
            }
            foreach (string source in new[] { "Active", "Automatic", "Always", "Disabled", "N/A", "AER" })
            {
                quality.Text = source;
                Check(quality.Text != source || source == "AER", "SSW state was not translated");
            }
            ethernet.Text = "USB Speed";
        });
        Check(overlay._forceDrawAll && root.IsDirty && !panel._forceDrawAll && !panel._gridMain.IsDirty,
            "overlay caption invalidated the wrong owner");
        Check(ethernet.Text == "USB 速率" && game.Posts == 0 && root.ChildReads == reads,
            "overlay state change queued a later frame or rescanned the page");
        Check((string)status.Font == "latin-font" && (string)quality.Font == "latin-font",
            "numeric PC state or AER retained the translation font");
        overlay._forceDrawAll = root.IsDirty = false;
        game.Invoke(() => status.Text = "unknown connection");
        Check(status.Text == "unknown connection" && !overlay._forceDrawAll, "unknown overlay status requested redraw");
        game.Invoke(() => VdHsMod.UiLocalize.LocalizeControl(new Xenko.UI.Controls.CheckBox {
            Content = new Xenko.UI.Controls.TextBlock { Text = "Suppress system gestures" } }));
        Check(panel._forceDrawAll && !overlay._forceDrawAll, "panel caption invalidated overlay owner");
    }
    private static void FontRestoreTest(string scenario)
    {
        var panel = Panel();
        var game = (MockGame)VirtualDesktop.Mobile.InputManager._game;
        var label = Label(panel._gridMain, "Low");
        Table();
        MockGame.RequireUiThread = true;
        Call(Localize, "Tick");
        Check((string)label.Font == "cjk-font", "initial translation did not use Chinese font");
        if (scenario == "font-pending")
        {
            MockGame.RequireUiThread = false;
            label.Text = "High";
            label.Text = "Medium";
            MockGame.RequireUiThread = true;
            game.Drain();
            Check(label.Text == "中" && (string)label.Font == "cjk-font", "queued own translation restored the wrong font");
        }
        game.Invoke(() => label.Text = "Godlike");
        Check(label.Text == "Godlike" && (string)label.Font == "latin-font", "preserved meme name retained Chinese font");
        game.Invoke(() => label.Text = "High");
        game.Invoke(() => label.Text = "AER");
        Check(label.Text == "AER" && (string)label.Font == "latin-font", "technical name retained Chinese font");
        game.Invoke(() => label.Text = "No");
        game.Invoke(() => label.Text = "3.0");
        Check(label.Text == "3.0" && (string)label.Font == "latin-font", "numeric state retained Chinese font");
        game.Invoke(() => label.Text = "High");
        game.Invoke(() => label.Text = "unknown status");
        Check(label.Text == "unknown status" && (string)label.Font == "latin-font", "unknown state retained Chinese font");
    }
    public static int Main(string[] args)
    {
        string s = args[0];
        if (s == "localization") LocalizationTest();
        else if (s == "templates") TemplateTest();
        else if (s == "table") TableTest();
        else if (s == "language") LanguageTest();
        else if (s == "font-restore" || s == "font-pending") FontRestoreTest(s);
        else if (s.StartsWith("overlay", StringComparison.Ordinal)) OverlayTest(s);
        else if (s == "cache" || s == "font-late" || s == "new-toggle" || s == "new-root" || s == "new-tab" ||
            s == "no-dispatcher" || s == "event" || s == "off-thread" || s == "pending-dispose" || s == "unknown-text" ||
            s == "resource-late" || s == "resource-alias" || s == "incomplete-page" ||
            s == "quality-cycle" || s == "latest-caption" || s == "repeat-caption" ||
            s == "repeat-async" || s == "repeat-font") LifecycleTest(s);
        else ToggleTest(s);
        Console.WriteLine("PASS " + s);
        return 0;
    }
}
