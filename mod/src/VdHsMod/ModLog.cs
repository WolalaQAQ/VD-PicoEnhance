using System;
using System.Reflection;

namespace VdHsMod
{
    /// <summary>
    /// Logging. In this app only <c>Android.Util.Log</c> reaches logcat
    /// (observed): plain <c>Console.WriteLine</c> does not. Mono.Android
    /// is always loaded in the process, so we reach it by name through
    /// reflection - no compile-time reference.
    /// </summary>
    internal static class ModLog
    {
        internal const string Tag = "vdhs-mod";

        private static MethodInfo _info;
        private static MethodInfo _warn;
        private static MethodInfo _error;
        private static bool _init;
        private static bool _verbose = true;

        public static void Init()
        {
            if (_init) return;
            _init = true;
            try
            {
                Type logType = Type.GetType("Android.Util.Log, Mono.Android");
                if (logType == null)
                {
                    try { logType = Assembly.Load("Mono.Android").GetType("Android.Util.Log"); }
                    catch { }
                }
                if (logType != null)
                {
                    _info = Find(logType, "Info");
                    _warn = Find(logType, "Warn");
                    _error = Find(logType, "Error");
                }
                _verbose = PayloadConfig.EnvBool("VDHS_VERBOSE", true);
            }
            catch
            {
                // logging must never break the host
            }
        }

        private static MethodInfo Find(Type t, string name)
        {
            return t.GetMethod(
                name,
                BindingFlags.Public | BindingFlags.Static,
                null,
                new[] { typeof(string), typeof(string) },
                null);
        }

        public static void Info(string message) { Emit(_info, "I", message); }
        public static void Warn(string message) { Emit(_warn, "W", message); }
        public static void Error(string message) { Emit(_error, "E", message); }
        public static void Verbose(string message) { if (_verbose) Info(message); }

        private static void Emit(MethodInfo sink, string level, string message)
        {
            try
            {
                if (sink != null)
                {
                    sink.Invoke(null, new object[] { Tag, message });
                    return;
                }
            }
            catch
            {
                // fall through to stderr
            }
            try { Console.Error.WriteLine("[" + Tag + "/" + level + "] " + message); } catch { }
        }
    }
}
