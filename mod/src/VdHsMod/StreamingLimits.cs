using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Threading;

namespace VdHsMod
{
    // VD's shared bitrate formula extrapolates beyond 1 and its quality enum
    // already contains the higher tiers. Change UI bounds, not code or packets.
    internal static class StreamingLimits
    {
        private const BindingFlags Members = BindingFlags.Instance | BindingFlags.Static |
            BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        private static object _root, _disposedRoot, _panel, _streaming, _settings, _user, _mobile, _streamer;
        private static object _vrSlider, _desktopSlider, _qualitySlider;
        private static object _overlay, _maxBitrate;
        private static Action _removeOverlayHandler;
        private static Type _ranges;
        private static bool _loaded, _applying;
        private static int _dirty;
        private static float? _savedVR, _savedDesktop, _savedQuality;
        private static readonly List<Action> _unsubscribe = new List<Action>();

        internal static void Tick(object game)
        {
            try
            {
                if (!_loaded) Load();
                var panel = Get(game, "ControlPanel");
                var root = Get(panel, "_gridMain");
                if (root == null || ReferenceEquals(root, _disposedRoot)) return;
                if (!ReferenceEquals(root, _root) && !Bind(game, panel, root)) return;
                BindOverlay(game);
                Flush();
            }
            catch (Exception ex) { ModLog.Error("streaming limits: " + ex.Message); }
        }

        private static bool Bind(object game, object panel, object root)
        {
            var tabs = Get(panel, "_tabPanels") as Array;
            if (tabs == null || tabs.Length != 7) return false;
            var settings = tabs.GetValue(4);
            var streaming = tabs.GetValue(5);
            var quality = Get(streaming, "_sldQuality");
            var vr = Get(streaming, "_sldBitrate");
            var desktop = Get(settings, "_sldBitrate");
            if (quality == null || vr == null || desktop == null) return false;
            var user = Default("SharedUserSettings");
            var mobile = Default("SharedMobileSettings");
            var streamer = Default("SharedStreamerSettings");
            if (user == null || mobile == null || streamer == null) return false;

            Release();
            _panel = panel;
            _streaming = streaming;
            _settings = settings;
            _user = user;
            _mobile = mobile;
            _streamer = streamer;
            _ranges = GestureToggle.FindType("Xenko.VR.HmdResolutionTypeExtensions");
            if (_ranges == null) throw new InvalidOperationException("VD bitrate table is unavailable");
            _vrSlider = vr;
            _desktopSlider = desktop;
            _qualitySlider = quality;
            Apply(true);
            _root = root;
            _disposedRoot = null;

            // Native handlers queue their UI updates first. Refresh our limit
            // afterwards, using the real measurement and the widened bounds.
            EventHandler refresh = (sender, args) =>
                game.GetType().GetMethod("BeginInvoke", new[] { typeof(Action) }).Invoke(game,
                    new object[] { (Action)(() =>
                    {
                        if (!ReferenceEquals(root, _root)) return;
                        try { Apply(false); }
                        catch (Exception ex) { ModLog.Error("streaming limits refresh: " + ex.Message); }
                    }) });
            Subscribe(_mobile, "MeasuredBandwidthChanged", refresh);
            Subscribe(_mobile, "FoveatedStreamingChanged", refresh);
            Subscribe(_streamer, "ActiveCodecChanged", refresh);
            // Dragging a native slider can raise an event each frame. Persist
            // once on the existing UI tick instead of doing file I/O per event.
            EventHandler save = (sender, args) => { if (!_applying) Interlocked.Exchange(ref _dirty, 1); };
            Subscribe(_user, "VRBitrateLimitChanged", save);
            Subscribe(_user, "DesktopBitrateLimitChanged", save);
            Subscribe(_user, "VRGraphicsQualityChanged", save);
            EventHandler<EventArgs> disposing = (sender, args) =>
            {
                _disposedRoot = root;
                try { Flush(); }
                catch (Exception ex) { ModLog.Error("streaming limits save: " + ex.Message); }
                finally { Release(); }
            };
            var gameType = game.GetType();
            gameType.GetMethod("add_DeviceDisposing").Invoke(game, new object[] { disposing });
            _unsubscribe.Add(() => gameType.GetMethod("remove_DeviceDisposing").Invoke(game, new object[] { disposing }));
            ModLog.Info("streaming limits: native sliders bound");
            return true;
        }

        private static void Apply(bool restore)
        {
            _applying = true;
            try
            {
                Set(_vrSlider, "Maximum", Maximum(true));
                Set(_desktopSlider, "Maximum", Maximum(false));
                float qualityMaximum = Convert.ToSingle(Get(_qualitySlider, "Maximum"));
                foreach (object value in Enum.GetValues(_user.GetType().GetProperty("VRGraphicsQuality").PropertyType))
                    qualityMaximum = Math.Max(qualityMaximum, Convert.ToSingle(value));
                Set(_qualitySlider, "Maximum", qualityMaximum);
                // The stock tab constructor clamps its widgets before we bind.
                // Restore extended selections saved by this module afterwards.
                SetSelection("VRBitrateLimit", _vrSlider, restore ? _savedVR : null);
                SetSelection("DesktopBitrateLimit", _desktopSlider, restore ? _savedDesktop : null);
                float selected = restore && _savedQuality.HasValue ? _savedQuality.Value :
                    Convert.ToSingle(Get(_user, "VRGraphicsQuality"));
                selected = Math.Min(selected, qualityMaximum);
                var qualityType = _user.GetType().GetProperty("VRGraphicsQuality").PropertyType;
                if (Convert.ToSingle(Get(_user, "VRGraphicsQuality")) != selected)
                    Set(_user, "VRGraphicsQuality", Enum.ToObject(qualityType, (int)selected));
                Set(_qualitySlider, "Value", selected);
                UpdateLimit(_vrSlider, true);
                UpdateLimit(_desktopSlider, false);
                Invoke(_streaming, "UpdateVRBitrate");
                // SettingsTab's overload computes the label from its real slider.
                Invoke(_settings, "OnDesktopBitrateValueChanged", _desktopSlider, null);
                UpdateOverlayMaximum();
                Set(_panel, "_forceDrawAll", true);
            }
            finally { _applying = false; }
        }

        private static void SetSelection(string property, object slider, float? saved)
        {
            float current = Convert.ToSingle(Get(_user, property));
            float value = Math.Min(saved ?? current, Convert.ToSingle(Get(slider, "Maximum")));
            if (current != value) Set(_user, property, value);
            Set(slider, "Value", value);
        }

        private static float Maximum(bool vr)
        {
            int minimum, maximum;
            Range(vr, out minimum, out maximum);
            return (3f * maximum - minimum) / (maximum - minimum);
        }

        private static void Range(bool vr, out int minimum, out int maximum)
        {
            var hmd = Get(_mobile, "HmdType");
            minimum = (int)Invoke(_ranges, vr ? "GetMinVRBitrate" : "GetMinDesktopBitrate", hmd);
            maximum = (int)(vr ? Invoke(_ranges, "GetMaxVRBitrate", hmd,
                Get(_streamer, "ActiveCodec"), Get(_mobile, "FoveatedStreaming")) :
                Invoke(_ranges, "GetMaxDesktopBitrate", hmd));
        }

        private static void UpdateLimit(object slider, bool vr)
        {
            int bandwidth = (int)Get(_mobile, "MeasuredBandwidth");
            int minimum, maximum;
            Range(vr, out minimum, out maximum);
            // InverseLerp in the original UI can cap at 1. The shared bitrate
            // formula does not. Both still use exactly the same real bandwidth.
            float limit = bandwidth > 0 ? Math.Max(0.00001f, (bandwidth - (float)minimum) / (maximum - minimum)) : 0f;
            Set(slider, "Limit", limit);
        }

        private static void Subscribe(object target, string name, EventHandler handler)
        {
            var type = target.GetType();
            type.GetMethod("add_" + name).Invoke(target, new object[] { handler });
            _unsubscribe.Add(() => type.GetMethod("remove_" + name).Invoke(target, new object[] { handler }));
        }

        private static void BindOverlay(object game)
        {
            var overlay = Get(game, "PerformanceOverlay");
            var maximum = Get(overlay, "_tbMaxBitrate");
            if (ReferenceEquals(maximum, _maxBitrate)) return;
            _removeOverlayHandler?.Invoke();
            _removeOverlayHandler = null;
            _overlay = overlay;
            _maxBitrate = maximum;
            if (maximum == null) return;

            // VD rewrites this label when the overlay opens or its codec/format
            // changes. Finish the correction in the same UI-thread setter, not
            // on the next poll. The current-bitrate label remains VD-owned.
            var type = maximum.GetType();
            EventHandler changed = (sender, args) => UpdateOverlayMaximum();
            type.GetMethod("add_TextChanged").Invoke(maximum, new object[] { changed });
            _removeOverlayHandler = () => type.GetMethod("remove_TextChanged").Invoke(maximum, new object[] { changed });
            UpdateOverlayMaximum();
        }

        private static void UpdateOverlayMaximum()
        {
            if (_maxBitrate == null) return;
            int bandwidth = (int)Get(_mobile, "MeasuredBandwidth");
            if (bandwidth == 0) return; // Match VD's unmeasured/disconnected state.
            int minimum, maximum;
            Range(true, out minimum, out maximum);
            float mbps = Math.Min(bandwidth, 3f * maximum) / 1000000f;
            string text = mbps.ToString(mbps >= 10f ? "0' Mbps'" : "0.0' Mbps'", CultureInfo.InvariantCulture);
            if ((string)Get(_maxBitrate, "Text") == text) return;
            Set(_maxBitrate, "Text", text);
            Set(_overlay, "_forceDrawAll", true);
        }

        private static void Release()
        {
            _removeOverlayHandler?.Invoke();
            _removeOverlayHandler = null;
            _overlay = _maxBitrate = null;
            foreach (var remove in _unsubscribe) remove();
            _unsubscribe.Clear();
            _root = null;
        }

        private static string FlagPath()
        {
            return Path.Combine(Path.GetDirectoryName(typeof(StreamingLimits).Assembly.Location), "streaming_limits.txt");
        }

        private static void Load()
        {
            _loaded = true;
            string path = FlagPath();
            if (!File.Exists(path)) return;
            foreach (string line in File.ReadAllText(path).Split('\n'))
            {
                string[] pair = line.Trim().Split('=');
                if (pair.Length != 2) continue;
                float value;
                if (!float.TryParse(pair[1], NumberStyles.Float, CultureInfo.InvariantCulture, out value) ||
                    float.IsNaN(value) || float.IsInfinity(value)) continue;
                if (pair[0] == "vr" && value >= 0) _savedVR = value;
                if (pair[0] == "desktop" && value >= 0) _savedDesktop = value;
                if (pair[0] == "quality") _savedQuality = value;
            }
        }

        private static void Flush()
        {
            if (Interlocked.Exchange(ref _dirty, 0) != 0) Save();
        }

        private static void Save()
        {
            _savedVR = Convert.ToSingle(Get(_user, "VRBitrateLimit"));
            _savedDesktop = Convert.ToSingle(Get(_user, "DesktopBitrateLimit"));
            _savedQuality = Convert.ToSingle(Get(_user, "VRGraphicsQuality"));
            File.WriteAllText(FlagPath(), "vr=" + _savedVR.Value.ToString("R", CultureInfo.InvariantCulture) +
                "\ndesktop=" + _savedDesktop.Value.ToString("R", CultureInfo.InvariantCulture) +
                "\nquality=" + _savedQuality.Value.ToString("R", CultureInfo.InvariantCulture) + "\n");
        }

        private static object Default(string name)
        {
            var type = GestureToggle.FindType("VirtualDesktop.Mobile." + name);
            return type?.GetProperty("Default", Members)?.GetValue(null);
        }

        private static object Get(object target, string name) { return GestureToggle.Get(target, name); }
        private static void Set(object target, string name, object value)
        {
            var type = target.GetType();
            var property = type.GetProperty(name, Members);
            if (property != null) property.SetValue(target, value);
            else type.GetField(name, Members).SetValue(target, value);
        }
        private static object Invoke(object target, string name, params object[] args)
        {
            var type = target as Type ?? target.GetType();
            foreach (var method in type.GetMethods(Members))
                if (method.Name == name && method.GetParameters().Length == args.Length)
                    return method.Invoke(target is Type ? null : target, args);
            throw new InvalidOperationException("VD method unavailable: " + name);
        }
    }
}
