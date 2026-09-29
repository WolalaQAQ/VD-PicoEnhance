using System;
using System.IO;
using System.Reflection;

namespace VdHsMod
{
    /// <summary>
    /// Loads the mod's optional satellite assemblies straight from the payload
    /// directory:
    ///
    ///   * <c>0Harmony.dll</c> (only if the harmony backend is used), and
    ///   * optionally a pre-patched <c>Xenko.OpenXR.dll</c> (redirect backend).
    ///
    /// We deliberately avoid <c>AppDomain.AssemblyResolve</c>. On the official
    /// APK's netcore Mono (.NET Android, core = System.Private.CoreLib) the
    /// mod's netstandard2.0 assembly cannot resolve <c>System.ResolveEventHandler</c>
    /// through the netstandard facade: referencing it makes Loader.Start throw
    /// TypeLoadException on device. Explicit <c>Assembly.Load(byte[])</c> registers
    /// the assembly by identity in the app's load context, which is all our own
    /// references (HarmonyLib, the patched Xenko.OpenXR) need.
    /// </summary>
    internal static class AssemblyRedirect
    {
        private static string _payloadDir;

        /// <summary>Directory the payload (libvdhs.so / VdHsMod.dll) was staged in.</summary>
        public static string PayloadDir
        {
            get
            {
                if (_payloadDir != null) return _payloadDir;
                try
                {
                    string env = Environment.GetEnvironmentVariable("VDHS_PAYLOAD_DIR");
                    if (!string.IsNullOrEmpty(env)) _payloadDir = env;
                }
                catch { }
                if (string.IsNullOrEmpty(_payloadDir))
                {
                    try
                    {
                        string loc = typeof(Loader).Assembly.Location;
                        if (!string.IsNullOrEmpty(loc))
                            _payloadDir = Path.GetDirectoryName(loc);
                    }
                    catch { }
                }
                if (string.IsNullOrEmpty(_payloadDir))
                {
                    try { _payloadDir = AppDomain.CurrentDomain.BaseDirectory; } catch { }
                }
                return _payloadDir;
            }
        }

        /// <summary>
        /// Loads <c>&lt;payload&gt;/&lt;simpleName&gt;.dll</c> if present, returning the
        /// registered assembly (or null). Safe to call repeatedly: a second
        /// Assembly.Load of the same bytes returns the already-loaded identity.
        /// </summary>
        public static Assembly TryLoadFromPayload(string simpleName)
        {
            try
            {
                string dir = PayloadDir;
                if (string.IsNullOrEmpty(dir)) return null;
                string candidate = dir + "/" + simpleName + ".dll";
                ModLog.Verbose("LoadFromPayload <- " + candidate);
                // Each strategy lives in its own method: on the trimmed BCL a
                // missing API fails when the *method* is JIT-compiled, so
                // isolating them keeps one missing method from hiding the rest.
                Assembly asm = TryLoadFrom(candidate);
                if (asm != null) return asm;
                return TryLoadBytes(candidate);
            }
            catch (Exception ex)
            {
                ModLog.Warn("LoadFromPayload(" + simpleName + "): " + ex.GetType().Name + " " + ex.Message);
                return null;
            }
        }

        private static Assembly TryLoadFrom(string path)
        {
            try { return LoadFromCore(path); }
            catch (Exception ex)
            {
                ModLog.Warn("Assembly.LoadFrom(" + path + "): " + ex.GetType().Name + " " + ex.Message);
                return null;
            }
        }

        private static Assembly LoadFromCore(string path) { return Assembly.LoadFrom(path); }

        private static Assembly TryLoadBytes(string path)
        {
            try { return LoadBytesCore(path); }
            catch (Exception ex)
            {
                ModLog.Warn("Assembly.Load(bytes " + path + "): " + ex.GetType().Name + " " + ex.Message);
                return null;
            }
        }

        private static Assembly LoadBytesCore(string path)
        {
            if (!File.Exists(path)) return null;
            return Assembly.Load(File.ReadAllBytes(path));
        }

        /// <summary>
        /// Kept for Loader.Start's call order. Resolution is explicit now
        /// (see TryLoadFromPayload), so there is no event to install.
        /// </summary>
        public static void Install()
        {
            ModLog.Verbose("AssemblyRedirect: explicit payload loading (payload=" + PayloadDir + ")");
        }
    }
}
