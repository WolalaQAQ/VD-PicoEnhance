using System;
using System.IO;
using System.Reflection;
using System.Threading;

namespace VdHsMod
{
    /// <summary>
    /// Adds a "Suppress system gestures" row to Virtual Desktop's Input tab and
    /// keeps its state in the app-private <c>vdhs/system_gestures.txt</c>, which the
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
        private static object _installedRoot;
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
                    Mirror();
                }
                catch (Exception ex)
                {
                    ModLog.Error("GestureToggle mirror: " + ex.Message);
                }
                Thread.Sleep(500);
            }
        }

        private static void Mirror()
        {
            if (_checkBox == null) return;
            var game = FindGame();
            if (game == null) return;
            object val = null;
            Dispatch(game, () =>
            {
                Install(game);
                val = Get(_checkBox, "IsChecked");
            });
            if (!(val is bool)) return;
            int on = (bool)val ? 1 : 0;
            if (on == _lastWritten) return;

            var path = FlagPath();
            if (path == null) return;
            File.WriteAllText(path, on == 1 ? "1\n" : "0\n");
            _lastWritten = on;
            ModLog.Info("gesture toggle: " + (on == 1 ? "suppression ON" : "suppression OFF"));
        }

        private static bool TryInstall()
        {
            var game = FindGame();
            if (game == null) return false;
            bool added = false;
            Dispatch(game, () => added = Install(game));
            return added;
        }

        private static void Dispatch(object game, Action action)
        {
            var invoke = game.GetType().GetMethod("Invoke", new[] { typeof(Action) });
            if (invoke == null) throw new InvalidOperationException("VD UI dispatcher is unavailable");
            invoke.Invoke(game, new object[] { action });
        }

        private static bool Install(object game)
        {
            var panel = Get(game, "ControlPanel");
            if (panel == null) return false;
            var root = Get(panel, "_gridMain");
            if (root == null || (_installed && ReferenceEquals(root, _installedRoot))) return false;

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

            // This checkbox is the only user switch. Missing or invalid saved
            // state means OFF; displaying the row does not create a setting.
            bool initial = false;
            var path = FlagPath();
            if (path != null && File.Exists(path))
            {
                try
                {
                    // Match service.sh exactly: discard line endings, not spaces.
                    // An invalid saved value must mean OFF in both.
                    var saved = File.ReadAllText(path).Replace("\r", "").Replace("\n", "");
                    if (saved == "0" || saved == "1") initial = saved == "1";
                }
                catch { }
            }
            Set(box, "IsChecked", initial);
            _lastWritten = initial ? 1 : 0;

            add.Invoke(children, new[] { box });
            UiLocalize.LocalizeControl(box);
            _checkBox = box;
            _installedRoot = root;
            _installed = true;
            ModLog.Info("gesture toggle: added to " + parent.GetType().FullName +
                        " (children=" + CountChildren(children) + ")");
            return true;
        }

        internal static void EnqueueChildren(System.Collections.Generic.Queue<object> queue, object node, string member)
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

        internal static object FindGame()
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

        internal static object Get(object target, string member)
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

        internal static void TrySet(object target, string member, object value)
        {
            try { Set(target, member, value); } catch { }
        }
    }
}
