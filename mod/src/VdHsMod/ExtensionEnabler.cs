using System;
using System.Collections;
using System.Reflection;

namespace VdHsMod
{
    /// <summary>
    /// Reflection-only data injection. This is the part of the mod that can
    /// work with no IL patching at all, and is shared by every backend:
    ///
    ///   1. teach <c>System.EnumExtensions</c>'s per-enum name cache that the
    ///      runtime extension name maps to an Extension value, so VD's
    ///      TryParse succeeds; and
    ///   2. append that value to XR._picoExtensions, so the filtering loop in
    ///      InitializeAndroid keeps it.
    ///
    /// Both are pure data edits on live objects and are individually guarded.
    /// Note (unverified): step 1 assumes the name cache has already been
    /// initialised; GetValue(null) forces the static ctor if not.
    /// </summary>
    internal static class ExtensionEnabler
    {
        private const BindingFlags NF = BindingFlags.NonPublic | BindingFlags.Static;

        public static void Install()
        {
            InjectEnumName(VdReflection.ControllerExtension, VdReflection.ControllerExtensionValue);
            InjectEnumName(VdReflection.HandTrackingExtension, VdReflection.HandTrackingExtensionValue);
            AppendToPicoExtensions();
        }

        private static void InjectEnumName(string name, int value)
        {
            try
            {
                Type extType = VdReflection.ExtensionType;
                if (extType == null) { ModLog.Warn("enum inject: Extension type not found"); return; }

                Assembly core = VdReflection.LoadAssembly(VdReflection.CoreAssembly);
                Type ee = core == null ? null : core.GetType(VdReflection.EnumExtensionsTypeName);
                if (ee == null) { ModLog.Warn("enum inject: " + VdReflection.EnumExtensionsTypeName + " not found"); return; }

                Type cacheOpen = ee.GetNestedType(VdReflection.EnumCacheNestedName, BindingFlags.NonPublic);
                if (cacheOpen == null) { ModLog.Warn("enum inject: Cache`1 not found"); return; }

                Type cache = cacheOpen.MakeGenericType(extType);
                object enumValue = Enum.ToObject(extType, value);

                AddToDictionary(cache, "_valuesByName", name, enumValue);
                AddToDictionary(cache, "_valuesByNameIgnoreCase", name, enumValue);
                AddToDictionary(cache, "_namesByValue", enumValue, name);
                AddToDictionary(cache, "_displayNamesByValue", enumValue, name);

                ModLog.Info("enum inject: " + name + "=" + value + " -> " + cache.Name);
            }
            catch (Exception ex)
            {
                ModLog.Warn("InjectEnumName(" + name + "): " + ex.Message);
            }
        }

        private static void AddToDictionary(Type cache, string fieldName, object key, object value)
        {
            FieldInfo field = cache.GetField(fieldName, NF);
            if (field == null) { ModLog.Verbose("  no field " + fieldName); return; }
            object dict = field.GetValue(null); // forces the static ctor
            if (dict is IDictionary d)
            {
                d[key] = value; // Dictionary<,> implements the non-generic IDictionary
                return;
            }
            // Fallback: generic IDictionary<,> via reflection.
            var add = dict.GetType().GetMethod("set_Item");
            if (add != null)
            {
                add.Invoke(dict, new[] { key, value });
                return;
            }
            ModLog.Warn("  " + fieldName + " is not an IDictionary (" + dict.GetType() + ")");
        }

        private static void AppendToPicoExtensions()
        {
            try
            {
                Type xr = VdReflection.XrType;
                if (xr == null) { ModLog.Warn("picoExt: XR type not found"); return; }
                FieldInfo field = xr.GetField("_picoExtensions", NF);
                if (field == null) { ModLog.Warn("picoExt: _picoExtensions not found"); return; }

                Array current = (Array)field.GetValue(null);
                if (current == null) { ModLog.Warn("picoExt: null array"); return; }
                Array grown = AppendControllerExtension(current);
                if (ReferenceEquals(grown, current))
                {
                    ModLog.Info("picoExt: controller ext already present");
                    return;
                }
                field.SetValue(null, grown);
                ModLog.Info("picoExt: appended " + VdReflection.ControllerExtension + " (len "
                            + current.Length + " -> " + grown.Length + ")");
            }
            catch (Exception ex)
            {
                // static readonly field SetValue is the risky step on some runtimes;
                // the harmony backend adds a GetPlatformExtensions postfix instead.
                ModLog.Warn("AppendToPicoExtensions: " + ex.Message);
            }
        }

        /// <summary>
        /// Returns <paramref name="array"/> with the controller extension value
        /// appended, or the original array when it is null / already present.
        /// Used by both the field replacement and the Harmony postfix.
        /// </summary>
        public static Array AppendControllerExtension(Array array)
        {
            if (array == null) return null;
            Type elem = array.GetType().GetElementType();
            object value = Enum.ToObject(elem, VdReflection.ControllerExtensionValue);
            foreach (object item in array)
                if (Equals(item, value)) return array;
            Array grown = Array.CreateInstance(elem, array.Length + 1);
            Array.Copy(array, grown, array.Length);
            grown.SetValue(value, array.Length);
            return grown;
        }
    }
}
