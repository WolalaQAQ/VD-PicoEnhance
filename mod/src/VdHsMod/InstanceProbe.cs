using System;
using System.Threading;

namespace VdHsMod
{
    /// <summary>
    /// Diagnostic, independent of the backend. Waits for VD's
    /// XR._instance, then polls xrGetActiveInputDeviceTypePico through the
    /// native payload. The runtime only resolves that function when the
    /// instance was created with XR_PICO_android_controller_function_ext_enable,
    /// so the first logged value tells whether VD really enabled the extension:
    ///   activeInputType=0/1/2 -> enabled;  -11 -> gipa refused (not enabled).
    /// NativeBridge.ActiveInput logs transitions only.
    /// </summary>
    internal static class InstanceProbe
    {
        public static void Start()
        {
            try
            {
                var t = new Thread(Run) { IsBackground = true, Name = "vdhs-probe" };
                t.Start();
            }
            catch (Exception ex)
            {
                ModLog.Warn("InstanceProbe: thread start failed: " + ex.Message);
            }
        }

        // XR._supportedExtensions is exactly the set VD passed to xrCreateInstance.
        private static void LogSupported()
        {
            try
            {
                var f = VdReflection.XrType?.GetField("_supportedExtensions",
                    System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                var set = f?.GetValue(null) as System.Collections.IEnumerable;
                if (set == null) { ModLog.Warn("probe: _supportedExtensions unavailable"); return; }
                int n = 0; bool ctrl = false;
                foreach (object e in set)
                {
                    n++;
                    if (Convert.ToInt32(e) == VdReflection.ControllerExtensionValue) ctrl = true;
                }
                ModLog.Info("probe: VD enabled " + n + " exts, " + VdReflection.ControllerExtension + "=" + (ctrl ? 1 : 0));
            }
            catch (Exception ex) { ModLog.Warn("probe: _supportedExtensions: " + ex.Message); }
        }

        private static void Run()
        {
            try
            {
                ulong instance = 0;
                for (int i = 0; i < 240 && instance == 0; i++)   // ~120 s
                {
                    instance = VdReflection.GetInstance();
                    if (instance == 0) Thread.Sleep(500);
                }
                if (instance == 0) { ModLog.Warn("probe: XR._instance never set (120 s)"); return; }
                ModLog.Info("probe: XR._instance=0x" + instance.ToString("x"));
                LogSupported();
                while (true)
                {
                    NativeBridge.ActiveInput();
                    Thread.Sleep(500);
                }
            }
            catch (Exception ex)
            {
                ModLog.Warn("probe: " + ex.Message);
            }
        }
    }
}
