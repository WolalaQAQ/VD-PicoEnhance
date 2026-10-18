using System;
using System.IO;
using System.Reflection;
using System.Threading;

namespace VdHsMod
{
    /// <summary>
    /// Adds a "Suppress system gestures" row to Virtual Desktop's Input tab and
    /// keeps its state in <c>&lt;payload&gt;/system_gestures.txt</c>, which the
    /// module mirrors to the Shell payload.
    ///
    /// VD builds its tabs from a UI asset and looks its controls up by name
    /// (<c>VisualTreeHelper.FindVisualChildOfType&lt;CheckBox&gt;(grid, "chk...")</c>),
    /// so the row is created here at runtime and appended next to the existing
    /// checkboxes. Everything goes through reflection: no compile-time reference
    /// to VirtualDesktop.* or Xenko.* assemblies, and every step degrades to a
    /// no-op so the host process survives.
    ///
    /// The checkbox drives itself (Xenko toggles IsChecked on click); this class
    /// only reads it, so no delegate plumbing is needed.
    /// </summary>
    internal static class GestureToggle
    {
        private const string CheckBoxType = "Xenko.UI.Controls.CheckBox";
        private const string TextBlockType = "Xenko.UI.Controls.TextBlock";
        private const string RowName = "chkSuppressSystemGestures";
        private const string Label = "Suppress system gestures";

        private static bool _started;
        private static bool _installed;
        private static object _checkBox;
        private static int _lastWritten = -1;

        internal static void Start()
        {
            if (_started) return;
            _started = true;
            try
            {
                var t = new Thread(Poll) { IsBackground = true, Name = "vdhs-gesture-toggle" };
                t.Start();
            }
            catch (Exception ex)
            {
                ModLog.Error("GestureToggle.Start: " + ex.Message);
            }
        }

        /// <summary>Payload directory, derived from this assembly's own path.</summary>
        private static string FlagPath()
        {
            try
            {
                var loc = typeof(GestureToggle).Assembly.Location;
                if (string.IsNullOrEmpty(loc)) return null;
                return Path.Combine(Path.GetDirectoryName(loc), "system_gestures.txt");
            }
            catch { return null; }
        }

        private static void Poll()
        {
            // The Input tab only exists once VD has built its control panel.
            for (int i = 0; i < 900 && !_installed; i++)
            {
                try
                {
                    if (TryInstall()) ModLog.Info("gesture toggle: row installed");
                }
                catch (Exception ex)
                {
                    ModLog.Error("GestureToggle install: " + ex.Message);
                }
                if (!_installed) Thread.Sleep(2000);
            }

            // Mirror the checkbox into the flag file.
            for (;;)
            {
                try
                {
                    var box = _checkBox;
                    if (box != null)
                    {
                        var val = Get(box, "IsChecked");
                        int on = (val is bool b && b) ? 1 : 0;
                        if (on != _lastWritten)
                        {
                            var path = FlagPath();
                            if (path != null)
                            {
                                File.WriteAllText(path, on == 1 ? "1\n" : "0\n");
                                _lastWritten = on;
                                ModLog.Info("gesture toggle: " + (on == 1 ? "suppression ON" : "suppression OFF"));
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    ModLog.Error("GestureToggle mirror: " + ex.Message);
                }
                Thread.Sleep(500);
            }
        }

        private static bool TryInstall()
        {
            if (_installed) return false;

            var game = FindGame();
            if (game == null) return false;

            var panel = Get(game, "ControlPanel");
            if (panel == null) return false;

            var tabs = GetField(panel, "_tabPanels") as Array;
            if (tabs == null || tabs.Length < 4) return false;

            var inputTab = tabs.GetValue(3);
            if (inputTab == null) return false;

            var existing = GetField(inputTab, "_chkAutoHideHands");
            if (existing == null) return false;

            var parent = Get(existing, "VisualParent");
            if (parent == null) return false;

            var children = Get(parent, "Children");
            var add = children?.GetType().GetMethod("Add", new[] { FindType("Xenko.UI.UIElement") ?? typeof(object) });
            if (children == null || add == null)
            {
                ModLog.Error("gesture toggle: parent has no Children.Add (" + parent.GetType().FullName + ")");
                return false;
            }

            var cbType = FindType(CheckBoxType);
            var tbType = FindType(TextBlockType);
            if (cbType == null || tbType == null)
            {
                ModLog.Error("gesture toggle: Xenko UI types not found");
                return false;
            }

            var box = Activator.CreateInstance(cbType);
            var text = Activator.CreateInstance(tbType);
            Set(text, "Text", Label);
            Set(box, "Content", text);
            TrySet(box, "Name", RowName);

            // VD's checkboxes get their look from the UI asset (sprite providers,
            // content template, layout), not from a theme, so a bare instance
            // measures to zero and stays invisible. Copy the appearance of the
            // row we anchor to, including its label's text style.
            CopyAppearance(existing, box);
            var existingLabel = FirstVisualChild(existing);
            if (existingLabel != null) CopyAppearance(existingLabel, text);

            // EXPERIMENT: find a font that can actually draw Chinese. VD shows
            // Chinese somewhere (its "寻求帮助" button), so the UI must already
            // hold a CJK-capable SpriteFont; reuse that one.
            // ControlPanel is a game system; its UI root is the _gridMain field.
            var uiRoot = Get(panel, "_gridMain") ?? panel;
            ModLog.Info("gesture toggle: UI root = " + Describe(uiRoot));
            // Give the label a font that can draw non-Latin text, taken from VD's
            // own UI (its help button is Chinese and uses the same SpriteFont).
            var labelFont = FindCjkFont(uiRoot);
            if (labelFont != null) TrySet(text, "Font", labelFont);

            var path = FlagPath();
            if (path != null && File.Exists(path))
            {
                try
                {
                    var saved = File.ReadAllText(path).Trim();
                    TrySet(box, "IsChecked", saved == "1");
                }
                catch { }
            }

            add.Invoke(children, new[] { box });
            _checkBox = box;
            _installed = true;
            ModLog.Info("gesture toggle: added to " + parent.GetType().FullName +
                        " (children=" + CountChildren(children) + ")");
            return true;
        }

        /// <summary>
        /// Walks the UI tree looking for a TextBlock that already displays
        /// Chinese, and returns the SpriteFont it uses.
        /// </summary>
        private static object FindCjkFont(object root)
        {
            var queue = new System.Collections.Generic.Queue<object>();
            if (root != null) queue.Enqueue(root);
            int visited = 0;
            while (queue.Count > 0 && visited < 20000)
            {
                var node = queue.Dequeue();
                visited++;
                if (node == null) continue;

                var text = Get(node, "Text") as string;
                if (!string.IsNullOrEmpty(text) && HasCjk(text))
                {
                    var font = Get(node, "Font");
                    ModLog.Info("gesture toggle: UI text \"" + text + "\" uses font " + Describe(font));
                    if (font != null) return font;
                }

                EnqueueChildren(queue, node, "Children");
                EnqueueChildren(queue, node, "VisualChildren");
            }
            ModLog.Info("gesture toggle: walked " + visited + " UI nodes, no CJK text found");
            return null;
        }

        private static void EnqueueChildren(System.Collections.Generic.Queue<object> queue, object node, string member)
        {
            var children = Get(node, member);
            if (children == null) return;
            var count = Convert.ToInt32(children.GetType().GetProperty("Count")?.GetValue(children) ?? 0);
            var indexer = children.GetType().GetProperty("Item");
            if (indexer == null) return;
            for (int i = 0; i < count && i < 512; i++)
            {
                try { queue.Enqueue(indexer.GetValue(children, new object[] { i })); }
                catch { }
            }
        }

        private static bool HasCjk(string s)
        {
            foreach (var c in s)
                if (c >= 0x3400 && c <= 0x9FFF) return true;
            return false;
        }

        private static string Describe(object font)
        {
            if (font == null) return "(null)";
            var name = Get(font, "Name") as string;
            return font.GetType().FullName + (name != null ? " name=" + name : "");
        }

        private static object FirstVisualChild(object element)
        {
            try
            {
                var vc = Get(element, "VisualChildren");
                var idx = vc?.GetType().GetProperty("Item");
                if (vc == null) return null;
                var count = Convert.ToInt32(vc.GetType().GetProperty("Count")?.GetValue(vc) ?? 0);
                return count > 0 ? idx?.GetValue(vc, new object[] { 0 }) : null;
            }
            catch { return null; }
        }

        // Copies the members that decide how a control looks: sprite providers
        // (CheckBox/ToggleButton images), the content template, text styling and
        // layout. Anything null, read-only or type-mismatched is skipped.
        private static void CopyAppearance(object from, object to)
        {
            if (from == null || to == null) return;
            var toType = to.GetType();
            foreach (var p in from.GetType().GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                try
                {
                    if (!p.CanRead || !p.CanWrite) continue;
                    var name = p.Name;
                    bool wanted =
                        name.IndexOf("Image", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        name.IndexOf("Alignment", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        name.IndexOf("Template", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        name.IndexOf("TextSize", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        name == "Margin" || name == "Padding" ||
                        name == "Width" || name == "Height" ||
                        name == "Font" || name == "Foreground" || name == "Color" || name == "TextColor";
                    if (!wanted) continue;

                    var value = p.GetValue(from);
                    if (value == null) continue;

                    var target = toType.GetProperty(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                    if (target == null || !target.CanWrite) continue;
                    if (!target.PropertyType.IsInstanceOfType(value)) continue;
                    target.SetValue(to, value);
                }
                catch { }
            }
        }

        private static int CountChildren(object children)
        {
            try
            {
                var prop = children.GetType().GetProperty("Count");
                return prop != null ? Convert.ToInt32(prop.GetValue(children)) : -1;
            }
            catch { return -1; }
        }

        private static object FindGame()
        {
            var t = FindType("VirtualDesktop.Mobile.InputManager");
            if (t == null) return null;
            var f = t.GetField("_game", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            return f?.GetValue(null);
        }

        private static Type FindType(string fullName)
        {
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                try
                {
                    // The host runs dotnet/runtime's Mono, which does not carry
                    // the (string, bool) overload; the single-argument form is
                    // present and throws instead of returning null.
                    var t = asm.GetType(fullName);
                    if (t != null) return t;
                }
                catch { }
            }
            return null;
        }

        private static object Get(object target, string member)
        {
            if (target == null) return null;
            var t = target.GetType();
            var p = t.GetProperty(member, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (p != null) return p.GetValue(target);
            var f = t.GetField(member, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            return f?.GetValue(target);
        }

        private static object GetField(object target, string field)
        {
            if (target == null) return null;
            var f = target.GetType().GetField(field, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            return f?.GetValue(target);
        }

        private static void Set(object target, string member, object value)
        {
            if (target == null) return;
            var t = target.GetType();
            var p = t.GetProperty(member, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (p != null && p.CanWrite) { p.SetValue(target, value); return; }
            var f = t.GetField(member, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            f?.SetValue(target, value);
        }

        private static void TrySet(object target, string member, object value)
        {
            try { Set(target, member, value); } catch { }
        }
    }
}
