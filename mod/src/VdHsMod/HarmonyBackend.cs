using System;
using System.Reflection;

#if VDHS_HARMONY
using HarmonyLib;
#endif

namespace VdHsMod
{
    /// <summary>
    /// Runtime IL patching through the Harmony backend.
    ///
    ///   * <c>XR.IsControllerActive</c> prefix: return false while PICO reports
    ///     the active input device is the hand (2).
    ///   * <c>XR.GetPlatformExtensions</c> prefix: call the native
    ///     xrMarkApiClass(0x3b) at the exact point VD has finished
    ///     xrInitializeLoaderKHR and is about to enumerate/choose extensions.
    ///
    /// Availability is <c>unverified</c>: the official APK is AOT-compiled
    /// (E-026), and whether Harmony/MonoMod can detour an AOT method on this
    /// Mono 9 build is exactly what the device test must answer. If it cannot,
    /// the redirect backend is the fallback.
    ///
    /// The build is opt-in so the default artifact has no NuGet dependency:
    ///   dotnet build -p:VdHsHarmony=true
    /// and 0Harmony.dll must be staged next to this assembly.
    /// </summary>
    internal sealed class HarmonyBackend : IHotSwitchBackend
    {
        public string Name { get { return "harmony"; } }

#if VDHS_HARMONY
        public static bool HarmonyAvailable()
        {
            try
            {
                Assembly asm = VdReflection.LoadAssembly("0Harmony");
                return asm != null && asm.GetType("HarmonyLib.Harmony") != null;
            }
            catch (Exception ex)
            {
                ModLog.Warn("HarmonyAvailable: " + ex.Message);
                return false;
            }
        }

        public bool Install()
        {
            try
            {
                Type xr = VdReflection.XrType;
                if (xr == null) { ModLog.Warn("harmony: Xenko.OpenXR.XR not found"); return false; }

                var harmony = new Harmony("com.local.vdhsmod");
                int patched = 0;

                // Patch each target in isolation: one failure (e.g. a rejected
                // __result shape) must not cancel the others.
                MethodInfo isActive = VdReflection.NonPublicStatic(xr, "IsControllerActive");
                if (isActive != null && HotSwitchPatches.IsControllerActivePrefixMethod != null)
                {
                    try
                    {
                        harmony.Patch(isActive, prefix: new HarmonyMethod(HotSwitchPatches.IsControllerActivePrefixMethod));
                        patched++;
                        ModLog.Info("harmony: patched IsControllerActive");
                    }
                    catch (Exception ex) { ModLog.Error("harmony: IsControllerActive patch failed: " + ex.Message); }
                }

                MethodInfo getExt = VdReflection.NonPublicStatic(xr, "GetPlatformExtensions");
                if (getExt != null && HotSwitchPatches.GetPlatformExtensionsPrefixMethod != null)
                {
                    try
                    {
                        harmony.Patch(getExt,
                            prefix: new HarmonyMethod(HotSwitchPatches.GetPlatformExtensionsPrefixMethod),
                            postfix: HotSwitchPatches.GetPlatformExtensionsPostfixMethod == null
                                ? null
                                : new HarmonyMethod(HotSwitchPatches.GetPlatformExtensionsPostfixMethod));
                        patched++;
                        ModLog.Info("harmony: patched GetPlatformExtensions");
                    }
                    catch (Exception ex) { ModLog.Error("harmony: GetPlatformExtensions patch failed: " + ex.Message); }
                }

                // Extension list/name data injection is still needed (and works
                // even if the GetPlatformExtensions patch above failed).
                ExtensionEnabler.Install();

                return patched > 0;
            }
            catch (Exception ex)
            {
                ModLog.Error("harmony install failed: " + ex);
                ModLog.Warn("harmony failed; set backend=redirect or dataonly");
                return false;
            }
        }
#else
        public static bool HarmonyAvailable() { return false; }

        public bool Install()
        {
            ModLog.Warn("harmony backend not compiled in (rebuild with -p:VdHsHarmony=true); "
                        + "falling back to dataonly");
            return new DataOnlyBackend().Install();
        }
#endif
    }
}
