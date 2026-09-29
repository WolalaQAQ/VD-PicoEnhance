using System;

namespace VdHsMod
{
    /// <summary>
    /// Runtime configuration. Kept deliberately dumb so it can be changed on
    /// the device without rebuilding the module. Read only from the process
    /// environment: the app's trimmed BCL is missing System.IO methods (e.g.
    /// File.ReadAllLines, File.ReadAllBytes is unproven), so the NATIVE payload
    /// parses payload/backend.txt and exports it as VDHS_BACKEND.
    ///
    ///   env  VDHS_BACKEND  = auto|harmony|redirect|dataonly|none
    ///   env  VDHS_PAYLOAD_DIR = staged payload directory
    ///
    /// Backends:
    ///   auto      (default) redirect if a patched Xenko.OpenXR.dll is staged,
    ///             else harmony if compiled in and 0Harmony is present,
    ///             else dataonly.
    ///   redirect  preload a pre-patched Xenko.OpenXR.{patched.}dll (survives
    ///             the official APK's AOT images; see docs).
    ///   harmony   runtime IL patches via HarmonyLib (must build with
    ///             -p:VdHsHarmony=true and stage 0Harmony.dll).
    ///   dataonly  reflection-only data injection (enum name table + extension
    ///             array). Enables the PICO extension but cannot change
    ///             IsControllerActive's return value.
    ///   none      do nothing (Layer-1 injection smoke test).
    /// </summary>
    internal sealed class PayloadConfig
    {
        public string Backend = "auto";

        public static PayloadConfig Load()
        {
            var cfg = new PayloadConfig();
            try
            {
                string env = Environment.GetEnvironmentVariable("VDHS_BACKEND");
                if (!string.IsNullOrEmpty(env))
                    cfg.Backend = env.Trim().ToLowerInvariant();
            }
            catch (Exception ex)
            {
                ModLog.Warn("PayloadConfig.Load: " + ex.Message);
            }
            return cfg;
        }

        public static bool EnvBool(string name, bool fallback)
        {
            try
            {
                string v = Environment.GetEnvironmentVariable(name);
                if (string.IsNullOrEmpty(v)) return fallback;
                v = v.Trim().ToLowerInvariant();
                if (v == "0" || v == "false" || v == "no" || v == "off") return false;
                if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
            }
            catch { }
            return fallback;
        }
    }
}
