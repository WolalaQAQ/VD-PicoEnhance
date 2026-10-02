using System;
using System.IO;
using System.Reflection;

namespace VdHsMod
{
    /// <summary>
    /// Preloads a pre-patched <c>Xenko.OpenXR.dll</c> before VD first references
    /// it. Mono caches assemblies by identity, so a later
    /// <c>mono_assembly_load("Xenko.OpenXR")</c> returns ours and the store copy
    /// is never used.
    ///
    /// Why a separate artifact and not Harmony: the OFFICIAL APK is AOT
    /// ("normal", 172 libaot-*.so). A Harmony/MonoMod detour may or may
    /// not take over an AOT-compiled method. A replaced assembly whose MVID
    /// differs from the AOT image is rejected by the AOT loader and JITed
    /// instead, so the IL patch is guaranteed to run (inferred, must be tested).
    ///
    /// Artifact: <c>&lt;payload&gt;/Xenko.OpenXR.patched.dll</c>, produced from the
    /// patch pipeline PLUS an MVID bump so the
    /// official libaot-Xenko.OpenXR.dll.so no longer matches.
    /// </summary>
    internal sealed class RedirectBackend : IHotSwitchBackend
    {
        public const string ArtifactName = "Xenko.OpenXR.patched.dll";

        public string Name { get { return "redirect"; } }

        public static string ArtifactPath
        {
            get
            {
                string dir = AssemblyRedirect.PayloadDir;
                return string.IsNullOrEmpty(dir) ? null : Path.Combine(dir, ArtifactName);
            }
        }

        public static bool ArtifactPresent()
        {
            try
            {
                string p = ArtifactPath;
                return p != null && File.Exists(p);
            }
            catch { return false; }
        }

        public bool Install()
        {
            try
            {
                if (!ArtifactPresent())
                {
                    ModLog.Warn("redirect: " + ArtifactName + " not staged");
                    return false;
                }

                // If VD already pulled Xenko.OpenXR out of the store, this is a
                // lost cause for this launch - report it instead of pretending.
                foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
                {
                    try
                    {
                        if (a.GetName().Name == VdReflection.XenkoOpenXrAssembly)
                        {
                            ModLog.Warn("redirect: " + VdReflection.XenkoOpenXrAssembly
                                        + " already loaded before redirect; patch inactive this run");
                            return false;
                        }
                    }
                    catch { }
                }

                byte[] bytes = File.ReadAllBytes(ArtifactPath);
                Assembly patched = Assembly.Load(bytes);
                ModLog.Info("redirect: preloaded " + patched.FullName + " (" + bytes.Length + " bytes)");

                // Belt and suspenders: if VD somehow uses the store copy, the
                // data injection still enables the extension (but not the
                // IsControllerActive switch, which lives in the patched DLL).
                ExtensionEnabler.Install();
                return patched != null;
            }
            catch (Exception ex)
            {
                ModLog.Error("redirect install failed: " + ex);
                return false;
            }
        }
    }
}
