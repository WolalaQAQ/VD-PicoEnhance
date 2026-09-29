using System;
using System.Reflection;

namespace VdHsMod
{
    /// <summary>
    /// Every handle into VD is resolved here, by name. Nothing in this assembly
    /// holds a compile-time reference to Xenko.*, VirtualDesktop.* or
    /// Mono.Android.* (design §3.1).
    ///
    /// All type/field/method names below are <c>observed</c> in
    /// VD 1.34.22.0 (E-023 / decompiled Xenko.OpenXR).
    /// </summary>
    internal static class VdReflection
    {
        public const string XenkoOpenXrAssembly = "Xenko.OpenXR";
        public const string XrTypeName = "Xenko.OpenXR.XR";

        public const string CoreAssembly = "VirtualDesktop.Core";
        public const string EnumExtensionsTypeName = "System.EnumExtensions";
        public const string EnumCacheNestedName = "Cache`1";

        // E-023: the two PICO extensions and the enum slots the v4 patch used.
        public const string ControllerExtension = "XR_PICO_android_controller_function_ext_enable";
        public const string HandTrackingExtension = "XR_PICO_hand_tracking";
        public const int ControllerExtensionValue = 118;
        public const int HandTrackingExtensionValue = 119;

        private static Type _xr;
        private static Type _extension;
        private static FieldInfo _instanceField;

        public static Assembly LoadAssembly(string simpleName)
        {
            try
            {
                foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
                {
                    try { if (asm.GetName().Name == simpleName) return asm; }
                    catch { }
                }
            }
            catch { }
            // Optional satellites (0Harmony) are not in the app's store: load
            // them explicitly from the payload directory before asking the
            // default resolver, which has no AssemblyResolve handler here.
            try
            {
                var fromPayload = AssemblyRedirect.TryLoadFromPayload(simpleName);
                if (fromPayload != null) return fromPayload;
            }
            catch { }
            try { return Assembly.Load(simpleName); }
            catch (Exception ex)
            {
                ModLog.Warn("LoadAssembly(" + simpleName + "): " + ex.Message);
                return null;
            }
        }

        public static Type XrType
        {
            get
            {
                if (_xr != null) return _xr;
                try
                {
                    var asm = LoadAssembly(XenkoOpenXrAssembly);
                    if (asm != null) _xr = asm.GetType(XrTypeName);
                }
                catch (Exception ex) { ModLog.Warn("XrType: " + ex.Message); }
                return _xr;
            }
        }

        public static Type ExtensionType
        {
            get
            {
                if (_extension != null) return _extension;
                try
                {
                    var asm = LoadAssembly(XenkoOpenXrAssembly);
                    if (asm != null) _extension = asm.GetType("Xenko.OpenXR.Extension");
                }
                catch (Exception ex) { ModLog.Warn("ExtensionType: " + ex.Message); }
                return _extension;
            }
        }

        public static FieldInfo InstanceField
        {
            get
            {
                if (_instanceField != null) return _instanceField;
                var xr = XrType;
                if (xr == null) return null;
                _instanceField = xr.GetField("_instance", BindingFlags.NonPublic | BindingFlags.Static);
                return _instanceField;
            }
        }

        /// <summary>XrInstance as a raw ulong, or 0 before InitializeAndroid ran.</summary>
        public static ulong GetInstance()
        {
            try
            {
                var f = InstanceField;
                if (f == null) return 0;
                object v = f.GetValue(null);
                if (v is IntPtr ip) return (ulong)ip.ToInt64();
                if (v == null) return 0;
                return (ulong)Convert.ToInt64(v);
            }
            catch (Exception ex)
            {
                ModLog.Verbose("GetInstance: " + ex.Message);
                return 0;
            }
        }

        public static MethodInfo NonPublicStatic(Type type, string name)
        {
            try { return type == null ? null : type.GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static); }
            catch { return null; }
        }
    }
}
