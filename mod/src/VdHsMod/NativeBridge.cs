using System;
using System.Runtime.InteropServices;

namespace VdHsMod
{
    /// <summary>
    /// Bridge to the shared native payload <c>libvdhs.so</c>.
    ///
    /// The payload is the SAME artifact the patched-Xenko.OpenXR.dll route
    /// loads: that route's patched Xenko.OpenXR.dll calls <c>vdhs_mark</c>, and
    /// the Zygisk route dlopens it as the injection payload.
    ///
    /// We deliberately do NOT use [DllImport("libvdhs")] here. On the official
    /// APK's Mono (dotnet/runtime netcore Mono):
    ///   * <c>mono_dllmap_insert</c> is a stub that calls g_assert_not_reached(),
    ///     so the Zygisk route cannot install a dllmap;
    ///   * the app loads its libraries in the classloader linker namespace, and
    ///     this mod's native payload runs in the default namespace, so a
    ///     DllImport("libvdhs") lookup cannot find the staged .so either.
    /// Instead the native payload hands us the raw entry points through
    /// <see cref="SetPointers"/>, and we build callable delegates from them.
    ///
    /// Every call is guarded: a missing pointer degrades to a no-op.
    /// </summary>
    internal static class NativeBridge
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int ActiveInputFn(ulong instance);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int MarkLoadedFn();

        private static ActiveInputFn _activeInput;
        private static MarkLoadedFn _markLoaded;

        private static bool _broken;
        private static int _lastType = -2;

        /// <summary>
        /// Called by the native payload right before Loader.Start(). Raw
        /// addresses come from libvdhs.so's own exported symbols.
        /// </summary>
        public static void SetPointers(IntPtr activeInput, IntPtr markLoaded)
        {
            ModLog.Init(); // native calls this before Loader.Start, so init logging here
            try
            {
                if (activeInput != IntPtr.Zero)
                    _activeInput = (ActiveInputFn)Marshal.GetDelegateForFunctionPointer(activeInput, typeof(ActiveInputFn));
                if (markLoaded != IntPtr.Zero)
                    _markLoaded = (MarkLoadedFn)Marshal.GetDelegateForFunctionPointer(markLoaded, typeof(MarkLoadedFn));
                ModLog.Info("NativeBridge.SetPointers activeInput=" + activeInput.ToString("x")
                            + " markLoaded=" + markLoaded.ToString("x"));
            }
            catch (Exception ex)
            {
                ModLog.Warn("NativeBridge.SetPointers failed: " + ex.Message);
            }
        }

        /// <summary>
        /// Active input device type: 0 HMD, 1 controller, 2 hand.
        /// Returns -1 when unavailable (no instance yet / native missing).
        /// </summary>
        public static int ActiveInput()
        {
            if (_broken || _activeInput == null) return -1;
            try
            {
                ulong instance = VdReflection.GetInstance();
                if (instance == 0) return -1;
                int t = _activeInput(instance);
                if (t != _lastType)
                {
                    _lastType = t;
                    ModLog.Info("activeInputType=" + t);
                }
                return t;
            }
            catch (Exception ex)
            {
                _broken = true;
                ModLog.Warn("vdhs_active_input unavailable: " + ex.Message);
                return -1;
            }
        }

        /// <summary>
        /// Calls xrMarkApiClass(0x3b) through the system forward loader against
        /// the runtime VD has already initialised. Safe to call repeatedly.
        /// </summary>
        public static int MarkLoaded()
        {
            if (_broken || _markLoaded == null) return -1;
            try
            {
                int rc = _markLoaded();
                ModLog.Info("mark_loaded rc=" + rc);
                return rc;
            }
            catch (Exception ex)
            {
                ModLog.Warn("vdhs_mark_loaded unavailable: " + ex.Message);
                return -1;
            }
        }
    }
}
