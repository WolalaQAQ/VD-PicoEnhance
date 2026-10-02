using System;

namespace VdHsMod
{
    /// <summary>
    /// The single entry point both loaders call.
    ///
    /// The patched-Xenko.OpenXR.dll route calls <c>VdHsMod.Loader.Start()</c>
    /// through a one-line pin baked into the assembly store.
    /// The Zygisk route opens this assembly through the Mono embedding API and
    /// invokes <c>Loader.Start()</c> with no arguments and no return value.
    ///
    /// Contract: Start() must never throw. Every step is wrapped;
    /// a failure degrades to a no-op so the host process (VD) survives.
    /// </summary>
    public static class Loader
    {
        private static bool _started;

        /// <summary>Idempotent. Safe to call from any managed thread.</summary>
        public static void Start()
        {
            if (_started)
            {
                ModLog.Info("Start() called again; ignoring.");
                return;
            }
            _started = true;

            try
            {
                ModLog.Init();
                ModLog.Info("VdHsMod booting (assembly=" + typeof(Loader).Assembly.GetName().Version + ")");

                // 1) Let the mod resolve its own optional satellite assemblies
                //    (0Harmony) and the pre-patched Xenko.OpenXR.dll from the
                //    payload directory, without touching the app's store loader.
                AssemblyRedirect.Install();

                // 2) Pick a backend and apply it. BackendFactory never throws.
                var config = PayloadConfig.Load();
                var backend = BackendFactory.Create(config);
                ModLog.Info("backend=" + backend.Name + " (requested=" + config.Backend + ")");

                bool ok = backend.Install();
                ModLog.Info("backend install " + (ok ? "succeeded" : "FAILED (host left untouched)"));

                InstanceProbe.Start();
            }
            catch (Exception ex)
            {
                // Last-resort guard: a mod bug must not abort the shared process.
                ModLog.Error("Loader.Start unhandled: " + ex);
            }
        }

        /// <summary>Reset for tests only; not used at runtime.</summary>
        internal static void ResetForTests()
        {
            _started = false;
        }
    }
}
