using System;
using System.Reflection;

#if VDHS_HARMONY
namespace VdHsMod
{
    /// <summary>
    /// Patch bodies referenced by <see cref="HarmonyBackend"/>. They only need
    /// to be *methods*; Harmony resolves them by reflection, so no Harmony type
    /// appears in their signatures.
    ///
    /// Target signatures (observed):
    ///   static bool  Xenko.OpenXR.XR.IsControllerActive(nint session, int index)
    ///   static Extension[] Xenko.OpenXR.XR.GetPlatformExtensions(VirtualDesktop.Interfaces.Platform platform)
    /// </summary>
    internal static class HotSwitchPatches
    {
        public static readonly MethodInfo IsControllerActivePrefixMethod =
            typeof(HotSwitchPatches).GetMethod(nameof(IsControllerActivePrefix),
                BindingFlags.NonPublic | BindingFlags.Static);

        public static readonly MethodInfo GetPlatformExtensionsPrefixMethod =
            typeof(HotSwitchPatches).GetMethod(nameof(GetPlatformExtensionsPrefix),
                BindingFlags.NonPublic | BindingFlags.Static);

        public static readonly MethodInfo GetPlatformExtensionsPostfixMethod =
            typeof(HotSwitchPatches).GetMethod(nameof(GetPlatformExtensionsPostfix),
                BindingFlags.NonPublic | BindingFlags.Static);

        /// <summary>
        /// Prefix for IsControllerActive. Must have the single special argument
        /// <c>out bool __result</c>: when the active input device is the hand we
        /// set false and return false (skip the original); otherwise we return
        /// true and let VD's own logic decide.
        /// </summary>
        private static bool IsControllerActivePrefix(out bool __result)
        {
            __result = false;
            try
            {
                if (NativeBridge.ActiveInput() == 2)
                {
                    __result = false;
                    return false; // hands active -> controller not active
                }
            }
            catch (Exception ex)
            {
                ModLog.Verbose("IsControllerActivePrefix: " + ex.Message);
            }
            return true; // fall through to the original
        }

        /// <summary>
        /// Prefix for GetPlatformExtensions. Runs after VD's
        /// xrInitializeLoaderKHR has succeeded and before it enumerates
        /// extension properties, which is the exact window xrMarkApiClass must
        /// hit (the v9 ordering, E-025).
        /// </summary>
        private static void GetPlatformExtensionsPrefix()
        {
            NativeBridge.MarkLoaded();
        }

        /// <summary>
        /// Postfix for GetPlatformExtensions. Appends the controller extension
        /// value to the returned array. Declared as <c>ref Array</c> because the
        /// real return type (a private enum array) is not referenceable; the
        /// concrete Extension[] is assignable to Array. Also covers the case
        /// where replacing the static readonly _picoExtensions field fails.
        /// </summary>
        private static void GetPlatformExtensionsPostfix(ref Array __result)
        {
            try
            {
                __result = ExtensionEnabler.AppendControllerExtension(__result);
            }
            catch (Exception ex)
            {
                ModLog.Warn("GetPlatformExtensionsPostfix: " + ex.Message);
            }
        }
    }
}
#endif
