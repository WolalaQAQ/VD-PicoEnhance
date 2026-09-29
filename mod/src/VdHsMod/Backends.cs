using System;

namespace VdHsMod
{
    /// <summary>A loader-agnostic way to install the hot-switch behaviour.</summary>
    internal interface IHotSwitchBackend
    {
        string Name { get; }
        /// <returns>true when the backend installed something usable.</returns>
        bool Install();
    }

    internal sealed class NoopBackend : IHotSwitchBackend
    {
        public string Name { get { return "none"; } }
        public bool Install()
        {
            ModLog.Info("none backend: injection smoke test only; host untouched.");
            return true;
        }
    }

    /// <summary>
    /// Data injection only. Enables the PICO extension through VD's own
    /// filtering loop but cannot make IsControllerActive return false, so it is
    /// a diagnostic step, not a working hot-switch.
    /// </summary>
    internal sealed class DataOnlyBackend : IHotSwitchBackend
    {
        public string Name { get { return "dataonly"; } }
        public bool Install()
        {
            ExtensionEnabler.Install();
            // No Harmony hook, so mark has to happen before VD initialises the
            // runtime. Whether the runtime keeps the mark across VD's own
            // xrInitializeLoaderKHR is unverified (design §5.2).
            NativeBridge.MarkLoaded();
            ModLog.Warn("dataonly: extension list patched; IsControllerActive NOT changed (needs harmony/redirect)");
            return true;
        }
    }

    internal static class BackendFactory
    {
        public static IHotSwitchBackend Create(PayloadConfig config)
        {
            string requested = config == null || string.IsNullOrEmpty(config.Backend) ? "auto" : config.Backend;
            switch (requested)
            {
                case "none": return new NoopBackend();
                case "dataonly": return new DataOnlyBackend();
                case "redirect": return new RedirectBackend();
                case "harmony":
                    // Load 0Harmony from the payload before HarmonyBackend.Install
                    // is JIT-compiled; there is no AssemblyResolve fallback (§3 #6).
                    if (!HarmonyBackend.HarmonyAvailable())
                    {
                        ModLog.Warn("harmony requested but 0Harmony not loadable -> dataonly");
                        return new DataOnlyBackend();
                    }
                    return new HarmonyBackend();
                case "auto":
                default:
                    return Auto();
            }
        }

        private static IHotSwitchBackend Auto()
        {
            if (RedirectBackend.ArtifactPresent()) return new RedirectBackend();
#if VDHS_HARMONY
            if (HarmonyBackend.HarmonyAvailable()) return new HarmonyBackend();
#endif
            ModLog.Info("auto: no redirect artifact, harmony unavailable -> dataonly");
            return new DataOnlyBackend();
        }
    }
}
