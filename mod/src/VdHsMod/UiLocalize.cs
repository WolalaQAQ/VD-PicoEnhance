using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace VdHsMod
{
    /// <summary>
    /// Translates Virtual Desktop's control panel.
    ///
    /// The table lives in &lt;payload&gt;/ui_zh.txt as "english&lt;TAB&gt;chinese"
    /// lines, so translations can be edited on the device without rebuilding
    /// anything and deleting the file turns the feature off. Matching is exact
    /// and whole-string, so dynamic text (computer names, times, percentages,
    /// bitrates, addresses) is never touched and simply stays as it is.
    ///
    /// VD's UI font is Xenko's OfflineRasterizedSpriteFont, which can rasterize
    /// glyphs on demand, but an element keeps showing its original glyphs until it
    /// is handed the instance that knows how to draw the new characters, so
    /// translated elements get that font too.
    /// </summary>
    internal static class UiLocalize
    {
        private static bool _started;
        private static Dictionary<string, string> _table;
        private static object _font;

        internal static void Start()
        {
            if (_started) return;
            _started = true;
            try
            {
                _table = LoadTable();
                if (_table == null || _table.Count == 0)
                {
                    ModLog.Info("ui_zh: no table found, localization off");
                    return;
                }
                ModLog.Info("ui_zh: table has " + _table.Count + " entries");
                new Thread(Loop) { IsBackground = true, Name = "vdhs-ui-zh" }.Start();
            }
            catch (Exception ex)
            {
                ModLog.Error("UiLocalize.Start: " + ex.Message);
            }
        }

        private static Dictionary<string, string> LoadTable()
        {
            var path = Path.Combine(PayloadDir(), "ui_zh.txt");
            var text = ReadAllText(path);
            if (text == null) { ModLog.Error("ui_zh: could not read " + path); return null; }

            var table = new Dictionary<string, string>();
            foreach (var raw in text.Split('\n'))
            {
                var line = raw.TrimEnd('\r');
                if (line.Length == 0 || line[0] == '#') continue;
                var tab = line.IndexOf('\t');
                if (tab <= 0) continue;
                table[line.Substring(0, tab)] = line.Substring(tab + 1);
            }
            return table;
        }

        /// <summary>
        /// The host runs .NET-for-Android's trimmed runtime, where convenience
        /// APIs such as File.ReadAllLines are removed. Try progressively simpler
        /// ones so the table still loads.
        /// </summary>
        private static string ReadAllText(string path)
        {
            try { return File.ReadAllText(path); }
            catch (Exception ex) { ModLog.Info("ui_zh: File.ReadAllText unavailable (" + ex.Message + ")"); }

            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var reader = new StreamReader(fs))
                    return reader.ReadToEnd();
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

        private static void Loop()
        {
            for (;;)
            {
                try { Apply(); }
                catch (Exception ex) { ModLog.Error("UiLocalize: " + ex.Message); }
                Thread.Sleep(2000);
            }
        }

        private static void Apply()
        {
            var game = GestureToggle.FindGame();
            if (game == null) return;
            var panel = GestureToggle.Get(game, "ControlPanel");
            if (panel == null) return;

            var uiRoot = GestureToggle.Get(panel, "_gridMain") ?? panel;

            // VD keeps several SpriteFont instances: the UI's own one has no CJK
            // glyphs at all, while the one behind its Chinese text does. Find that
            // one and remember it, so later passes do not need it to be on screen.
            if (_font == null) _font = GestureToggle.FindCjkFont(uiRoot);
            object font = _font;

            var queue = new Queue<object>();
            queue.Enqueue(uiRoot);
            var tabs = GestureToggle.Get(panel, "_tabPanels") as Array;
            if (tabs != null)
                foreach (var tab in tabs)
                    if (tab != null) queue.Enqueue(tab);
            int translated = 0, visited = 0;
            while (queue.Count > 0 && visited < 40000)
            {
                var node = queue.Dequeue();
                visited++;
                if (node == null) continue;

                var text = GestureToggle.Get(node, "Text") as string;
                if (!string.IsNullOrEmpty(text))
                {
                    // Prefer the font VD itself uses for Chinese text; any other
                    // UI font will do as a fallback.

                    string zh;
                    if (_table.TryGetValue(text, out zh))
                    {
                        Translate(node, "Text", zh, font);
                        translated++;
                    }
                }

                // Buttons and other content controls draw their label from
                // Content, which is separate from any TextBlock child.
                var content = GestureToggle.Get(node, "Content") as string;
                string zhContent;
                if (content != null && _table.TryGetValue(content, out zhContent))
                {
                    Translate(node, "Content", zhContent, font);
                    translated++;
                }

                GestureToggle.EnqueueChildren(queue, node, "Children");
                GestureToggle.EnqueueChildren(queue, node, "VisualChildren");
            }

            if (translated > 0)
            {
                ModLog.Info("ui_zh: translated " + translated + " labels (font=" + (font != null) + ")");
                if (font != null) DescribeFont(font);
            }
        }

        /// <summary>
        /// Reports which of a probe string's characters the font can actually
        /// draw, so we know whether the atlas simply lacks them.
        /// </summary>
        private static void DescribeFont(object font)
        {
            try
            {
                const string probe = "屏蔽系统手势设置";
                var isPresent = font.GetType().GetMethod("IsCharPresent");
                if (isPresent == null)
                {
                    ModLog.Info("ui_zh: font " + font.GetType().Name + " has no IsCharPresent");
                    return;
                }
                var missing = new System.Text.StringBuilder();
                foreach (var c in probe)
                {
                    var present = (bool)isPresent.Invoke(font, new object[] { c });
                    if (!present) missing.Append(c);
                }
                ModLog.Info("ui_zh: font " + font.GetType().Name + " missing chars: " +
                            (missing.Length == 0 ? "(none)" : missing.ToString()));
            }
            catch (Exception ex)
            {
                ModLog.Error("ui_zh: DescribeFont: " + ex.Message);
            }
        }

        /// <summary>
        /// Sets a translated string on a member, gives the element (and its
        /// visual children, which is where content controls draw their label)
        /// the CJK font, and forces a re-layout so the previous text stops being
        /// drawn underneath.
        /// </summary>
        private static void Translate(object node, string member, string value, object font)
        {
            if (font != null) GestureToggle.TrySet(node, "Font", font);
            GestureToggle.TrySet(node, member, value);
            ApplyFontToChildren(node, font, 0);
            Invalidate(node);
            Invalidate(GestureToggle.Get(node, "VisualParent"));
        }

        private static void ApplyFontToChildren(object node, object font, int depth)
        {
            if (font == null || node == null || depth > 3) return;
            var children = GestureToggle.Get(node, "VisualChildren");
            if (children == null) return;
            var count = Convert.ToInt32(children.GetType().GetProperty("Count")?.GetValue(children) ?? 0);
            var indexer = children.GetType().GetProperty("Item");
            if (indexer == null) return;
            for (int i = 0; i < count && i < 64; i++)
            {
                object child;
                try { child = indexer.GetValue(children, new object[] { i }); }
                catch { continue; }
                if (child == null) continue;
                GestureToggle.TrySet(child, "Font", font);
                ApplyFontToChildren(child, font, depth + 1);
            }
        }

        private static void Invalidate(object node)
        {
            if (node == null) return;
            // Xenko declares these protected internal, so they need the non-public
            // binding flags; without them the calls silently do nothing and the
            // previous text keeps being drawn underneath.
            foreach (var name in new[] { "InvalidateMeasure", "InvalidateArrange", "InvalidateVisual" })
            {
                try
                {
                    var method = node.GetType().GetMethod(name,
                        System.Reflection.BindingFlags.Instance |
                        System.Reflection.BindingFlags.Public |
                        System.Reflection.BindingFlags.NonPublic,
                        null, Type.EmptyTypes, null);
                    method?.Invoke(node, null);
                }
                catch { }
            }
        }

        private static string PayloadDir()
        {
            var loc = typeof(UiLocalize).Assembly.Location;
            return string.IsNullOrEmpty(loc) ? "/data/local/tmp" : Path.GetDirectoryName(loc);
        }
    }
}
