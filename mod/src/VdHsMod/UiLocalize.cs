using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Threading;

namespace VdHsMod
{
    // Initialize the cached page and library once. Subsequent caption changes
    // use TextBlock.TextChanged, not a timer or repeated UI-tree traversal.
    internal static class UiLocalize
    {
        private static bool _started;
        private static Dictionary<string, string> _table;
        private static object _game, _panel, _root, _font, _overlayRoot;
        private static MethodInfo _invoke, _beginInvoke, _checkAccess;
        private static readonly Dictionary<object, TextTarget> _targets = new Dictionary<object, TextTarget>();

        private sealed class TextTarget
        {
            internal object Node;
            internal object Owner, Root;
            internal object OriginalFont;
            internal string Translation;
            internal PropertyInfo Text, Font;
            internal MethodInfo RemoveHandler;
            internal EventHandler Handler;
            internal bool Writing;
        }

        internal static void Start()
        {
            if (_started) return;
            _started = true;
            try
            {
                // VD itself reads InstalledUICulture for its assistant language.
                // Its game thread uses InvariantCulture, not the system language.
                _table = LoadTable(CultureInfo.InstalledUICulture.Name);
                if (_table == null || _table.Count == 0) return;
                new Thread(Initialize) { IsBackground = true, Name = "vdhs-ui-init" }.Start();
            }
            catch (Exception ex) { ModLog.Error("ui_zh start: " + ex.Message); }
        }

        private static void Initialize()
        {
            try
            {
                for (int i = 0; i < 120; i++)
                {
                    if (Tick()) return;
                    Thread.Sleep(500);
                }
                ModLog.Info("ui_zh: page/font was not ready; waiting for the next device-created event");
            }
            catch (Exception ex) { ModLog.Error("ui_zh initialize: " + ex.Message); }
        }

        private static bool Tick()
        {
            if (_root != null && _overlayRoot != null) return true;
            var game = GestureToggle.FindGame();
            if (game == null) return false;
            if (_game == null)
            {
                var type = game.GetType();
                _invoke = type.GetMethod("Invoke", new[] { typeof(Action) });
                _beginInvoke = type.GetMethod("BeginInvoke", new[] { typeof(Action) });
                _checkAccess = type.GetMethod("CheckAccess", Type.EmptyTypes);
                if (_invoke == null || _beginInvoke == null || _checkAccess == null)
                    throw new InvalidOperationException("VD UI dispatcher is unavailable");
                _game = game;
                // DeviceCreated precedes LoadContent. Queue, rather than run,
                // the initializer so it sees the completed UI resource tree.
                type.GetMethod("add_DeviceCreated").Invoke(game, new object[] {
                    new EventHandler<EventArgs>((sender, args) => Queue(InitializeAfterLoad)) });
                type.GetMethod("add_DeviceDisposing").Invoke(game, new object[] {
                    new EventHandler<EventArgs>((sender, args) => ReleasePanel()) });
            }
            bool ready = false;
            _invoke.Invoke(game, new object[] { (Action)(() => ready = Apply()) });
            return ready;
        }

        private static void Queue(Action action)
        {
            _beginInvoke.Invoke(_game, new object[] { action });
        }

        private static void InitializeAfterLoad()
        {
            try { Apply(); }
            catch (Exception ex) { ModLog.Error("ui_zh reload: " + ex.Message); }
        }

        private static bool Apply()
        {
            if (!ApplyPanel()) return false;
            var overlay = GestureToggle.Get(_game, "PerformanceOverlay");
            var root = GestureToggle.Get(overlay, "_grid");
            if (root == null) return false;
            if (ReferenceEquals(root, _overlayRoot)) return true;

            var content = GestureToggle.Get(overlay, "Content");
            var pageType = GestureToggle.Get(_panel, "Library").GetType().Assembly.GetType("Xenko.Engine.UIPage");
            var get = content?.GetType().GetMethod("Get", new[] { typeof(Type), typeof(string) });
            var page = get?.Invoke(content, new object[] { pageType, "UI/PerformanceOverlay/Page" });
            if (page == null) return false;
            if (!ReferenceEquals(root, GestureToggle.Get(page, "RootElement")))
                throw new InvalidOperationException("Cached performance Page and active overlay do not share their root");

            _overlayRoot = root;
            var protectedNodes = new HashSet<object>();
            Protect(protectedNodes, overlay, "_tbFramerate", "_tbLatency", "_tbBitrate", "_tbMaxBitrate",
                "_tbWifi", "_tbGame", "_tbEncoding", "_tbNetworking", "_tbDecoding", "_tbResolution",
                "_tbTargetFramerate", "_tbGPU", "_tbCodec", "_tbRuntime");
            ProtectNamed(protectedNodes, root, "tbHeadset", "tbVersion");
            int translated = Visit(root, protectedNodes, true, overlay, root);
            if (translated > 0) Redraw(overlay, root);
            ModLog.Info("ui_zh: performance overlay initialized; " + translated + " captions translated, " +
                _targets.Count + " total text-change listeners; initialization worker finished");
            return true;
        }

        private static bool ApplyPanel()
        {
            var panel = GestureToggle.Get(_game, "ControlPanel");
            var library = GestureToggle.Get(panel, "Library");
            var content = GestureToggle.Get(panel, "Content");
            var tabs = GestureToggle.Get(panel, "_tabPanels") as Array;
            var assistant = GestureToggle.Get(GestureToggle.Get(panel, "_bottomPanel"), "_tbAssistant");
            var font = GestureToggle.Get(assistant, "Font");
            if (library == null || content == null || font == null || tabs == null || tabs.Length != 7) return false;
            foreach (var tab in tabs) if (tab == null) return false;

            var pageType = library.GetType().Assembly.GetType("Xenko.Engine.UIPage");
            var get = content.GetType().GetMethod("Get", new[] { typeof(Type), typeof(string) });
            if (pageType == null || get == null) throw new InvalidOperationException("VD cached-page API is unavailable");
            var page = get.Invoke(content, new object[] { pageType, "UI/ControlPanel/Page" });
            var root = GestureToggle.Get(page, "RootElement");
            if (root == null) return false;
            if (!ReferenceEquals(root, GestureToggle.Get(panel, "_gridMain")))
                throw new InvalidOperationException("Cached Page and active panel do not share their root");
            if (ReferenceEquals(root, _root)) return true;

            ReleasePanel();
            _panel = panel;
            _root = root;
            _font = font;
            int translated = Visit(root, DynamicNodes(panel), true);
            // Some cards were cloned during the tab constructors, before this
            // initializer. Only their fixed captions are targets, never names.
            var computers = GestureToggle.Get(tabs.GetValue(0), "_pnComputers");
            foreach (var card in Children(computers))
            {
                var protectedNodes = new HashSet<object>();
                ProtectNamed(protectedNodes, card, "tbName", "tbDescription", "tbStatus");
                translated += Visit(card, protectedNodes, true);
            }
            var files = GestureToggle.Get(tabs.GetValue(6), "_gridFiles");
            foreach (var card in Children(files))
                if (GestureToggle.Get(card, "Tag")?.GetType().Name == "VideoFile")
                    translated += Visit(Child(GestureToggle.Get(Child(GestureToggle.Get(card, "Children"), 4), "Children"), 1),
                        new HashSet<object>(), true);
            translated += Visit(GestureToggle.Get(tabs.GetValue(6), "_itemVideos"), new HashSet<object>(), true);

            // Translate library templates without attaching event delegates:
            // later UICloner copies inherit captions/fonts, not mod listeners.
            var templates = GestureToggle.Get(library, "UIElements") as IDictionary;
            if (templates != null)
                foreach (DictionaryEntry item in templates)
                {
                    string name = item.Key as string;
                    if (name == "itemVideoFolder" || name == "itemFolderButton") continue;
                    var protectedNodes = new HashSet<object>();
                    if (name == "itemComputer")
                        ProtectNamed(protectedNodes, item.Value, "tbName", "tbDescription", "tbStatus");
                    if (name == "itemVideoFile")
                    {
                        // VideosTab.VideoFile assigns the filename and progress
                        // by these resource positions, not named fields.
                        var children = GestureToggle.Get(item.Value, "Children");
                        ProtectChild(protectedNodes, Child(children, 3), 1);
                        ProtectChild(protectedNodes, Child(children, 4), 0);
                    }
                    translated += Visit(item.Value, protectedNodes, false);
                }
            if (translated > 0) Redraw();
            ModLog.Info("ui_zh: cached control-panel Page/Library initialized; " + translated + " captions translated, " +
                _targets.Count + " text-change listeners");
            return true;
        }

        // Called on VD's UI thread immediately after the mod creates a row.
        internal static void LocalizeControl(object control)
        {
            if (_root != null && Visit(control, new HashSet<object>(), true) > 0) Redraw();
        }

        private static int Visit(object root, HashSet<object> protectedNodes, bool listen, object owner = null, object surface = null)
        {
            int translated = 0;
            Walk(root, protectedNodes, node =>
            {
                if (_targets.ContainsKey(node)) return;
                var type = node.GetType();
                // Editable text is user input, even when it matches the table.
                if (type.FullName == "Xenko.UI.Controls.EditText") return;
                var text = type.GetProperty("Text");
                var font = type.GetProperty("Font");
                if (text == null || text.PropertyType != typeof(string) || !text.CanWrite ||
                    font == null || !font.CanWrite) return;
                var target = new TextTarget { Node = node, Text = text, Font = font,
                    Owner = owner ?? _panel, Root = surface ?? _root, OriginalFont = font.GetValue(node) };
                if (listen)
                {
                    var add = type.GetMethod("add_TextChanged");
                    target.RemoveHandler = type.GetMethod("remove_TextChanged");
                    if (add == null || target.RemoveHandler == null) return;
                    target.Handler = (sender, args) => CaptionChanged(target);
                    add.Invoke(node, new object[] { target.Handler });
                    _targets.Add(node, target);
                }
                if (Translate(target)) translated++;
            });
            return translated;
        }

        private static void CaptionChanged(TextTarget target)
        {
            if ((bool)_checkAccess.Invoke(_game, null))
            {
                if (!target.Writing) UpdateCaption(target);
            }
            // Async writers must not block waiting for the UI thread. Read the
            // latest value there, but finish UI-originated writes synchronously
            // before VD can draw the intermediate English caption.
            else Queue(() => UpdateCaption(target, true));
        }

        private static void UpdateCaption(TextTarget target, bool forceRedraw = false)
        {
            try
            {
                TextTarget current;
                if (_targets.TryGetValue(target.Node, out current) && ReferenceEquals(current, target) && Translate(target, forceRedraw))
                    Redraw(target.Owner, target.Root);
            }
            catch (Exception ex) { ModLog.Error("ui_zh caption: " + ex.Message); }
        }

        private static bool Translate(TextTarget target, bool forceRedraw = false)
        {
            var source = target.Text.GetValue(target.Node) as string;
            // A queued callback can observe our own completed translation.
            if (target.Translation != null && source == target.Translation) return forceRedraw;
            string value;
            if (source == null || !_table.TryGetValue(source, out value) || source == value)
            {
                if (target.Translation == null) return false;
                target.Translation = null;
                target.Font.SetValue(target.Node, target.OriginalFont);
                return true;
            }
            var font = target.Font.GetValue(target.Node);
            // A UI-thread setter cannot draw its intermediate English value.
            // Restore identical Chinese text without redrawing the same pixels
            // or invalidating the font again. Async writers may already have
            // exposed English before this callback, so they still redraw.
            bool redraw = forceRedraw || target.Translation != value || !ReferenceEquals(font, _font);
            target.Writing = true;
            try
            {
                target.Translation = value;
                if (!ReferenceEquals(font, _font)) target.Font.SetValue(target.Node, _font);
                target.Text.SetValue(target.Node, value);
            }
            finally { target.Writing = false; }
            return redraw;
        }

        private static void Redraw(object owner = null, object root = null)
        {
            GestureToggle.TrySet(root ?? _root, "IsDirty", true);
            GestureToggle.TrySet(owner ?? _panel, "_forceDrawAll", true);
        }

        private static void ReleasePanel()
        {
            foreach (var target in _targets.Values)
                target.RemoveHandler.Invoke(target.Node, new object[] { target.Handler });
            _targets.Clear();
            _panel = _root = _font = _overlayRoot = null;
        }

        private static HashSet<object> DynamicNodes(object panel)
        {
            var nodes = new HashSet<object>();
            var tabs = (Array)GestureToggle.Get(panel, "_tabPanels");
            Protect(nodes, tabs.GetValue(0), "_pnComputers", "_tbSSID", "_tbWifiInfo", "_tbAccountID",
                "_tbUsbInfo", "_tbRegistryWarning", "_tbTitle", "_tbMessage");
            Protect(nodes, tabs.GetValue(2), "_gridOculusGames", "_gridSteamGames");
            Protect(nodes, tabs.GetValue(4), "_tbBitrate", "_tbBrightness", "_tbMicVolume",
                "_tbRed", "_tbGreen", "_tbBlue", "_tbSimilarity", "_tbSmoothness");
            Protect(nodes, tabs.GetValue(5), "_tbResolution", "_tbGPU", "_tbBitrate", "_tbSharpening", "_tbGamma",
                "_tbRed", "_tbGreen", "_tbBlue", "_tbSimilarity", "_tbSmoothness", "_tbOpacity");
            Protect(nodes, tabs.GetValue(6), "_gridFiles", "_pnFolders", "_tbMessage");
            Protect(nodes, GestureToggle.Get(panel, "_topPanel"), "_tbTime", "_tbHeadsetBattery");
            Protect(nodes, GestureToggle.Get(panel, "_bottomPanel"), "_tbFrameRate", "_tbVideoFrameRate", "_tbVideoBitrate");
            return nodes;
        }

        private static void Protect(HashSet<object> nodes, object owner, params string[] members)
        {
            foreach (var member in members)
            {
                var node = GestureToggle.Get(owner, member);
                if (node != null) nodes.Add(node);
            }
        }

        private static object Child(object children, int index)
        {
            return children?.GetType().GetProperty("Item")?.GetValue(children, new object[] { index });
        }

        private static IEnumerable Children(object parent)
        {
            return (GestureToggle.Get(parent, "Children") as IEnumerable) ?? new object[0];
        }

        private static void ProtectChild(HashSet<object> nodes, object parent, int index)
        {
            var child = Child(GestureToggle.Get(parent, "Children"), index);
            if (child != null) nodes.Add(child);
        }

        private static void ProtectNamed(HashSet<object> nodes, object root, params string[] names)
        {
            Walk(root, new HashSet<object>(), node =>
            {
                var name = GestureToggle.Get(node, "Name") as string;
                foreach (var excluded in names) if (name == excluded) nodes.Add(node);
            });
        }

        private static void Walk(object root, HashSet<object> protectedNodes, Action<object> visit)
        {
            var seen = new HashSet<object>();
            var queue = new Queue<object>();
            if (root != null) queue.Enqueue(root);
            while (queue.Count > 0)
            {
                var node = queue.Dequeue();
                if (node == null || protectedNodes.Contains(node) || !seen.Add(node)) continue;
                if (node.GetType().FullName == "Xenko.UI.Controls.EditText") continue;
                visit(node);
                var content = GestureToggle.Get(node, "Content");
                if (content != null) queue.Enqueue(content);
                GestureToggle.EnqueueChildren(queue, node, "Children");
                GestureToggle.EnqueueChildren(queue, node, "VisualChildren");
            }
        }

        internal static Dictionary<string, string> LoadTable(string language)
        {
            if (language == null || !(language.Equals("zh", StringComparison.OrdinalIgnoreCase) ||
                language.StartsWith("zh-", StringComparison.OrdinalIgnoreCase))) return null;
            var path = Path.Combine(PayloadDir(), "ui_zh.txt");
            if (!File.Exists(path)) return null;
            var text = ReadAllText(path);
            if (text == null) return null;
            var table = new Dictionary<string, string>();
            foreach (var raw in text.Split('\n'))
            {
                var line = raw.TrimEnd('\r');
                if (line.Length == 0 || line[0] == '#') continue;
                var fields = line.Split('\t');
                if (fields.Length != 2 || fields[0].Length == 0) continue;
                table[fields[0].Replace("\\n", "\n")] = fields[1].Replace("\\n", "\n");
            }
            return table;
        }

        // Keep the existing trimmed-runtime file-reading paths.
        private static string ReadAllText(string path)
        {
            try { return File.ReadAllText(path); }
            catch (Exception ex) { ModLog.Info("ui_zh: File.ReadAllText unavailable (" + ex.Message + ")"); }
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var reader = new StreamReader(fs)) return reader.ReadToEnd();
            }
            catch (Exception ex) { ModLog.Info("ui_zh: StreamReader unavailable (" + ex.Message + ")"); }
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    var buffer = new byte[fs.Length];
                    int read = 0;
                    while (read < buffer.Length)
                    {
                        int n = fs.Read(buffer, read, buffer.Length - read);
                        if (n <= 0) break;
                        read += n;
                    }
                    return System.Text.Encoding.UTF8.GetString(buffer, 0, read);
                }
            }
            catch (Exception ex) { ModLog.Error("ui_zh: FileStream unavailable (" + ex.Message + ")"); }
            return null;
        }

        private static string PayloadDir()
        {
            var loc = typeof(UiLocalize).Assembly.Location;
            return string.IsNullOrEmpty(loc) ? "/data/local/tmp" : Path.GetDirectoryName(loc);
        }
    }
}
