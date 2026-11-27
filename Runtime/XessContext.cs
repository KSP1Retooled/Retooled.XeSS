using System;
using System.Collections.Generic;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace Retooled.XeSS;

/// <summary>An XeSS Super Resolution context on Unity's graphics device.</summary>
/// <remarks>
/// The SDK is not thread-safe and needs the graphics device, so every call runs as a plugin event on the render thread.
/// Methods queue those events and return at once. <see cref="Status"/> shows what the render thread has done so far.
///
/// Creating a context starts building the SDK's pipelines in the background. Call <see cref="Poll"/> until
/// <see cref="XessStatus.PipelineResult"/> is <see cref="XessResult.Success"/>, then <see cref="Initialize"/>.
/// Direct3D 11 and 12 are supported, and Vulkan when <see cref="VulkanDeviceReady"/>. Other
/// graphics APIs fail with <see cref="XessResult.ErrorUnsupported"/>.
/// </remarks>
public sealed class XessContext : IDisposable
{
    const int MaxNameLength = 256;

    static IntPtr eventFunction;

    readonly CommandBuffer commands = new() { name = "XeSS" };

    readonly byte[] nameBuffer = new byte[MaxNameLength];

    IntPtr native;

    /// <summary>Queues the creation of a context for the given output size, preset and flags.</summary>
    /// <remarks>
    /// The flags also decide which pipelines the SDK builds. The velocity scale converts the motion vectors passed to
    /// <see cref="QueueExecute"/> into pixels that point from the current position to the previous one.
    /// </remarks>
    public XessContext(Vector2Int outputSize, XessQuality quality, XessInitFlags flags, Vector2 velocityScale)
    {
        if (eventFunction == IntPtr.Zero)
            eventFunction = XessNative.RetooledXeSS_GetEventFunction();

        native = XessNative.RetooledXeSS_NewContext();
        XessNative.RetooledXeSS_SetInitParams(
            native,
            (uint)outputSize.x,
            (uint)outputSize.y,
            quality,
            flags,
            velocityScale.x,
            velocityScale.y
        );
        Issue(XessNative.Event.Create);
    }

    /// <summary>Queues the creation of a context whose motion vectors are already in pixels.</summary>
    public XessContext(Vector2Int outputSize, XessQuality quality, XessInitFlags flags)
        : this(outputSize, quality, flags, Vector2.one) { }

    /// <summary>The graphics API the plugin saw at device creation.</summary>
    public static GraphicsDeviceType Renderer => (GraphicsDeviceType)XessNative.RetooledXeSS_GetRenderer();

    /// <summary>
    /// Whether Unity's Vulkan device was created with the extensions and features XeSS needs. The plugin adds them
    /// when it is loaded before the device: by the editor at startup, or in a player by a Vulkan layer that calls the
    /// plugin's <c>RetooledVulkanIntercept</c> export. Without them a context on Vulkan fails with
    /// <see cref="XessResult.ErrorUnsupported"/>.
    /// </summary>
    public static bool VulkanDeviceReady => XessNative.RetooledXeSS_IsVulkanDeviceReady() != 0;

    /// <summary>The minimum level of SDK messages written to the Unity log, for contexts created afterwards.</summary>
    public static XessLogLevel LogLevel
    {
        set => XessNative.RetooledXeSS_SetLogLevel(value);
    }

    public XessStatus Status
    {
        get
        {
            ThrowIfDisposed();
            XessNative.RetooledXeSS_GetStatus(native, out XessNative.Status status);
            return new XessStatus(status);
        }
    }

    /// <summary>Loads the SDK runtime if needed and reads its version.</summary>
    /// <remarks>Fails with <see cref="XessResult.ErrorCantLoadLibrary"/> if libxess.dll is missing.</remarks>
    public static XessResult TryGetSdkVersion(out Version version)
    {
        XessResult result = XessNative.RetooledXeSS_GetSdkVersion(out int major, out int minor, out int patch);
        version = new Version(major, minor, patch);
        return result;
    }

    /// <summary>Queues the SDK's init call. It blocks the render thread if the pipelines are still building.</summary>
    public void Initialize() => Issue(XessNative.Event.Init);

    /// <summary>Queues a refresh of the pipeline status and the profiling records.</summary>
    public void Poll() => Issue(XessNative.Event.Poll);

    /// <summary>Records an upscale into a command buffer, using the oldest queued parameters when it runs.</summary>
    /// <remarks>
    /// The buffer may run every frame. Before each run, call <see cref="QueueExecute"/> exactly once. Runs without
    /// queued parameters, or before the context is initialized, do nothing. Record again after replacing the context.
    /// </remarks>
    public void RecordExecute(CommandBuffer buffer)
    {
        ThrowIfDisposed();
        buffer.IssuePluginEventAndData(eventFunction, (int)XessNative.Event.Execute, native);
    }

    /// <summary>Queues the parameters for the next recorded upscale to run.</summary>
    public void QueueExecute(in XessExecuteParams parameters)
    {
        ThrowIfDisposed();
        var request = new XessNative.ExecuteRequest
        {
            Color = parameters.Color,
            Velocity = parameters.Velocity,
            Depth = parameters.Depth,
            Output = parameters.Output,
            JitterX = parameters.Jitter.x,
            JitterY = parameters.Jitter.y,
            ResetHistory = parameters.ResetHistory ? 1u : 0u,
            InputWidth = (uint)parameters.InputSize.x,
            InputHeight = (uint)parameters.InputSize.y,
        };
        XessNative.RetooledXeSS_QueueExecute(native, ref request);
    }

    /// <summary>Queues a dump of the next <paramref name="frameCount"/> executed frames into an existing folder.</summary>
    /// <remarks>
    /// The SDK caches about 50 MB per frame in memory and writes the files after the last frame, blocking that frame.
    /// Files already in the folder are overwritten.
    /// </remarks>
    public void StartDump(string folder, int frameCount, XessDumpElements elements, int firstFrameIndex = 0)
    {
        ThrowIfDisposed();
        byte[] path = Encoding.UTF8.GetBytes(folder + "\0");
        XessNative.RetooledXeSS_SetDumpParams(native, path, (uint)firstFrameIndex, (uint)frameCount, elements);
        Issue(XessNative.Event.Dump);
    }

    /// <summary>The GPU timings of the newest profiled frame, as of the last poll.</summary>
    /// <remarks>Empty unless the context was created with <see cref="XessInitFlags.EnableProfiling"/>.</remarks>
    public List<(string Name, double Seconds)> ProfilingRecords()
    {
        ThrowIfDisposed();
        var records = new List<(string, double)>();
        int count = XessNative.RetooledXeSS_GetProfilingRecord(native, -1, nameBuffer, nameBuffer.Length, out _);
        for (int i = 0; i < count; i++)
        {
            XessNative.RetooledXeSS_GetProfilingRecord(native, i, nameBuffer, nameBuffer.Length, out double seconds);
            int length = Array.IndexOf(nameBuffer, (byte)0);
            records.Add((Encoding.UTF8.GetString(nameBuffer, 0, length < 0 ? nameBuffer.Length : length), seconds));
        }
        return records;
    }

    /// <summary>Queues the destruction of the context. The render thread frees it.</summary>
    public void Dispose()
    {
        if (native == IntPtr.Zero)
            return;

        Issue(XessNative.Event.Destroy);
        native = IntPtr.Zero;
        commands.Release();
    }

    void Issue(XessNative.Event pluginEvent)
    {
        ThrowIfDisposed();
        commands.Clear();
        commands.IssuePluginEventAndData(eventFunction, (int)pluginEvent, native);
        Graphics.ExecuteCommandBuffer(commands);
    }

    void ThrowIfDisposed()
    {
        if (native == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(XessContext));
    }
}
