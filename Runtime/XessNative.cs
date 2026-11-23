using System;
using System.Runtime.InteropServices;

namespace Retooled.XeSS;

/// <summary>Declarations for Retooled.XeSS.Native.dll, the native plugin in <c>Native~</c>.</summary>
internal static class XessNative
{
    const string Plugin = "Retooled.XeSS.Native";

    /// <summary>Render-thread events, handled by <c>OnRenderEvent</c> in Plugin.cpp.</summary>
    internal enum Event
    {
        Create = 0,
        Init = 1,
        Poll = 2,
        Dump = 3,
        Destroy = 4,
        Execute = 5,
    }

    /// <summary>Plugin.cpp's <c>Status</c>, field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Status
    {
        public XessStage Stage;
        public XessResult CreateResult;
        public XessResult BuildResult;
        public XessResult PipelineResult;
        public XessResult InitResult;
        public XessResult DriverResult;
        public XessResult ProfilingResult;
        public XessResult DumpResult;
        public XessResult ExecuteResult;
        public int ExecuteCount;
        public int XeFXMajor;
        public int XeFXMinor;
        public int XeFXPatch;
    }

    [DllImport(Plugin)]
    internal static extern int RetooledXeSS_GetRenderer();

    [DllImport(Plugin)]
    internal static extern XessResult RetooledXeSS_GetSdkVersion(out int major, out int minor, out int patch);

    [DllImport(Plugin)]
    internal static extern void RetooledXeSS_SetLogLevel(XessLogLevel level);

    [DllImport(Plugin)]
    internal static extern IntPtr RetooledXeSS_GetEventFunction();

    [DllImport(Plugin)]
    internal static extern IntPtr RetooledXeSS_NewContext();

    [DllImport(Plugin)]
    internal static extern void RetooledXeSS_SetInitParams(
        IntPtr context,
        uint width,
        uint height,
        XessQuality quality,
        XessInitFlags flags,
        float velocityScaleX,
        float velocityScaleY
    );

    /// <summary>Backend.h's <c>ExecuteRequest</c>, field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct ExecuteRequest
    {
        public IntPtr Color;
        public IntPtr Velocity;
        public IntPtr Depth;
        public IntPtr Output;
        public float JitterX;
        public float JitterY;
        public uint ResetHistory;
        public uint InputWidth;
        public uint InputHeight;
    }

    [DllImport(Plugin)]
    internal static extern void RetooledXeSS_QueueExecute(IntPtr context, ref ExecuteRequest request);

    [DllImport(Plugin)]
    internal static extern void RetooledXeSS_SetDumpParams(
        IntPtr context,
        byte[] nullTerminatedUtf8Path,
        uint firstFrame,
        uint frameCount,
        XessDumpElements elements
    );

    [DllImport(Plugin)]
    internal static extern void RetooledXeSS_GetStatus(IntPtr context, out Status status);

    [DllImport(Plugin)]
    internal static extern int RetooledXeSS_GetProfilingRecord(
        IntPtr context,
        int index,
        [Out] byte[] name,
        int nameCapacity,
        out double seconds
    );
}
