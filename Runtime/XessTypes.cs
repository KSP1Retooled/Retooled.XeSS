using System;

namespace Retooled.XeSS;

/// <summary>The SDK's <c>xess_result_t</c>, plus <see cref="NotRun"/> for a step that has not happened yet.</summary>
public enum XessResult
{
    NotRun = int.MaxValue,
    WarningNonexistingFolder = 1,
    WarningOldDriver = 2,
    Success = 0,
    ErrorUnsupportedDevice = -1,
    ErrorUnsupportedDriver = -2,
    ErrorUninitialized = -3,
    ErrorInvalidArgument = -4,
    ErrorDeviceOutOfMemory = -5,
    ErrorDevice = -6,
    ErrorNotImplemented = -7,
    ErrorInvalidContext = -8,
    ErrorOperationInProgress = -9,
    ErrorUnsupported = -10,
    ErrorCantLoadLibrary = -11,
    ErrorWrongCallOrder = -12,
    ErrorUnknown = -1000,
}

/// <summary>The SDK's <c>xess_quality_settings_t</c>.</summary>
/// <remarks>The input resolution for each preset comes from the SDK. Native AA renders at the output resolution.</remarks>
public enum XessQuality
{
    UltraPerformance = 100,
    Performance = 101,
    Balanced = 102,
    Quality = 103,
    UltraQuality = 104,
    UltraQualityPlus = 105,
    NativeAA = 106,
}

/// <summary>The SDK's <c>xess_init_flags_t</c>, plus the profiling flag from <c>xess_debug.h</c>.</summary>
[Flags]
public enum XessInitFlags : uint
{
    None = 0,
    HighResMotionVectors = 1 << 0,
    InvertedDepth = 1 << 1,
    ExposureScaleTexture = 1 << 2,
    ResponsivePixelMask = 1 << 3,
    NdcVelocity = 1 << 4,
    ExternalDescriptorHeap = 1 << 5,
    LdrInputColor = 1 << 6,
    JitteredMotionVectors = 1 << 7,
    AutoExposure = 1 << 8,

    /// <summary>Records GPU timings that <see cref="XessContext.ProfilingRecords"/> returns.</summary>
    EnableProfiling = 1u << 30,
}

/// <summary>The SDK's <c>xess_logging_level_t</c>.</summary>
public enum XessLogLevel
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
}

/// <summary>The SDK's <c>xess_dump_element_bits_t</c>.</summary>
[Flags]
public enum XessDumpElements : uint
{
    InputColor = 0x01,
    InputVelocity = 0x02,
    InputDepth = 0x04,
    InputExposureScale = 0x08,
    InputResponsivePixelMask = 0x10,
    Output = 0x20,
    History = 0x40,
    ExecutionParameters = 0x80,
    AllInputs = InputColor | InputVelocity | InputDepth | InputExposureScale | InputResponsivePixelMask
        | ExecutionParameters,
    All = 0x7FFFFFFF,
}

/// <summary>How far a context has come on the render thread.</summary>
public enum XessStage
{
    Pending = 0,
    Created = 1,
    Initialized = 2,
    Failed = -1,
}

/// <summary>A snapshot of a context's state, as last written by the render thread.</summary>
public readonly struct XessStatus
{
    public readonly XessStage Stage;

    public readonly XessResult CreateResult;

    /// <summary>The result of starting the pipeline build, which runs in the background.</summary>
    public readonly XessResult BuildResult;

    /// <summary>The pipeline build status at the last poll. Success once the pipelines are ready.</summary>
    public readonly XessResult PipelineResult;

    public readonly XessResult InitResult;

    /// <summary><see cref="XessResult.WarningOldDriver"/> on an outdated Intel driver.</summary>
    public readonly XessResult DriverResult;

    /// <summary>
    /// The first profiling query after init, if <see cref="XessInitFlags.EnableProfiling"/> was set.
    /// <see cref="XessResult.ErrorNotImplemented"/> means the runtime was built without profiling.
    /// </summary>
    public readonly XessResult ProfilingResult;

    /// <summary>The result of the last <see cref="XessContext.StartDump"/>.</summary>
    public readonly XessResult DumpResult;

    /// <summary>The result of the last upscale.</summary>
    public readonly XessResult ExecuteResult;

    /// <summary>How many upscales the render thread has run.</summary>
    public readonly int ExecuteCount;

    /// <summary>The Intel XeFX library version. Zero on other GPU vendors.</summary>
    public readonly Version XeFXVersion;

    internal XessStatus(in XessNative.Status status)
    {
        Stage = status.Stage;
        CreateResult = status.CreateResult;
        BuildResult = status.BuildResult;
        PipelineResult = status.PipelineResult;
        InitResult = status.InitResult;
        DriverResult = status.DriverResult;
        ProfilingResult = status.ProfilingResult;
        DumpResult = status.DumpResult;
        ExecuteResult = status.ExecuteResult;
        ExecuteCount = status.ExecuteCount;
        XeFXVersion = new Version(status.XeFXMajor, status.XeFXMinor, status.XeFXPatch);
    }

    public override string ToString() =>
        $"stage {Stage}, create {CreateResult}, build {BuildResult}, pipelines {PipelineResult}, init {InitResult}, "
        + $"driver {DriverResult}, profiling {ProfilingResult}, dump {DumpResult}, "
        + $"execute {ExecuteResult} x{ExecuteCount}, XeFX {XeFXVersion}";
}

/// <summary>The inputs of one upscale.</summary>
/// <remarks>
/// Textures are native texture pointers from <see cref="UnityEngine.Texture.GetNativeTexturePtr"/>. Color and output
/// share a format. Motion vectors are RGHalf in the units that the context's velocity scale converts to pixels.
/// </remarks>
public struct XessExecuteParams
{
    public IntPtr Color;

    public IntPtr Velocity;

    public IntPtr Depth;

    public IntPtr Output;

    /// <summary>The projection jitter in input pixels, each component within half a pixel.</summary>
    public UnityEngine.Vector2 Jitter;

    public bool ResetHistory;

    public UnityEngine.Vector2Int InputSize;
}
