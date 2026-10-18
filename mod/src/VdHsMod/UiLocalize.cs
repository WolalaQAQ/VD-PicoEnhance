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
        private static readonly Dictionary<string, string> _styles = new Dictionary<string, string>();
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

                // A third field is an optional style for the few labels that sit
                // on top of text they replaced: pad=N trailing spaces, spread=N
                // spaces between characters, size=X multiplies the font size.
                var key = line.Substring(0, tab);
                var rest = line.Substring(tab + 1);
                var tab2 = rest.IndexOf('\t');
                if (tab2 >= 0)
                {
                    _styles[key] = rest.Substring(tab2 + 1);
                    rest = rest.Substring(0, tab2);
                }
                table[key] = rest;
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
                        Translate(node, "Text", zh, font, StyleOf(text));
                        translated++;
                    }
                }

                // Buttons and other content controls draw their label from
                // Content, which is separate from any TextBlock child.
                var content = GestureToggle.Get(node, "Content") as string;
                string zhContent;
                if (content != null && _table.TryGetValue(content, out zhContent))
                {
                    Translate(node, "Content", zhContent, font, StyleOf(content));
                    translated++;
                }

                // A content control's Content is not part of Children, so a label
                // kept in a Content TextBlock would otherwise be walked straight
                // past and left in its original language, which is what put
                // English underneath the translated button labels.
                var contentObject = GestureToggle.Get(node, "Content");
                if (contentObject != null && !(contentObject is string)) queue.Enqueue(contentObject);

                GestureToggle.EnqueueChildren(queue, node, "Children");
                GestureToggle.EnqueueChildren(queue, node, "VisualChildren");
            }

            if (translated > 0)
            {
                ModLog.Info("ui_zh: translated " + translated + " labels (font=" + (font != null) + ")");
            }
        }

        /// <summary>
        /// Sets a translated string on a member, gives the element (and its
        /// visual children, which is where content controls draw their label)
        /// the CJK font, and forces a re-layout so the previous text stops being
        /// drawn underneath.
        /// </summary>
        private static void Translate(object node, string member, string value, object font, string style)
        {
            if (font != null) GestureToggle.TrySet(node, "Font", font);
            // Clearing first forces the text renderer to throw away the glyphs it
            // built for the previous string; assigning straight over the top can
            // leave them on screen underneath the new text.
            GestureToggle.TrySet(node, member, string.Empty);
            GestureToggle.TrySet(node, member, Decorate(value, style));
            // A styled label is one that sits on top of the text it replaced, so
            // it also gets an opaque background to hide what is underneath.
            if (style != null) Cover(node);
            ApplyFontToChildren(node, font, 0);
        }

        private static string StyleOf(string key)
        {
            string style;
            return _styles.TryGetValue(key, out style) ? style : null;
        }

        /// <summary>
        /// Some labels are drawn on top of the text they replaced, so they need a
        /// wider box to hide it: pad adds trailing spaces, spread puts spaces
        /// between the characters (which keeps a short label centred).
        /// </summary>
        private static string Decorate(string value, string style)
        {
            if (string.IsNullOrEmpty(style)) return value;

            var result = value;

            var spread = Field(style, "spread=");
            if (spread > 0)
            {
                var builder = new System.Text.StringBuilder();
                for (int i = 0; i < value.Length; i++)
                {
                    builder.Append(value[i]);
                    if (i < value.Length - 1) builder.Append(new string(' ', (int)spread));
                }
                result = builder.ToString();
            }

            var pad = Field(style, "pad=");
            if (pad > 0) result += new string(' ', (int)pad);

            // vpad=N adds N blank lines above and below, which makes the label
            // taller without touching the pre-baked glyph size.
            var vpad = Field(style, "vpad=");
            if (vpad > 0)
            {
                var blank = new string('\n', (int)vpad);
                result = blank + result + blank;
            }

            return result;
        }

        private static float Field(string style, string name)
        {
            var at = style.IndexOf(name, StringComparison.OrdinalIgnoreCase);
            if (at < 0) return 0;
            var start = at + name.Length;
            var end = start;
            while (end < style.Length && (char.IsDigit(style[end]) || style[end] == '.')) end++;
            float parsed;
            return float.TryParse(style.Substring(start, end - start),
                System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out parsed) ? parsed : 0;
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

        private static void Cover(object node)
        {
            try
            {
                var parent = GestureToggle.Get(node, "VisualParent");
                var background = parent == null ? null : GestureToggle.Get(parent, "BackgroundColor");
                if (background != null) GestureToggle.TrySet(node, "BackgroundColor", background);
            }
            catch { }
        }

        private static string PayloadDir()
        {
            var loc = typeof(UiLocalize).Assembly.Location;
            return string.IsNullOrEmpty(loc) ? "/data/local/tmp" : Path.GetDirectoryName(loc);
        }
    }
}
