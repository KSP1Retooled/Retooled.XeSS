using System;
using System.Collections;
using System.IO;
using System.Text;
using UnityEngine;

namespace Retooled.XeSS;

/// <summary>Checks that XeSS works on this device and which debug features the runtime has.</summary>
/// <remarks>
/// <see cref="Run"/> creates a context at screen size, waits for the pipelines, initializes it, and tries profiling
/// and dumping. It logs one report and keeps it in <see cref="LastReport"/>. Nothing is executed, so nothing is drawn.
/// </remarks>
public static class XessDiagnostics
{
    const string Tag = "[XeSS] ";

    const float TimeoutSeconds = 60f;

    /// <summary>The report of the last finished run, or null.</summary>
    public static string LastReport { get; private set; }

    /// <summary>Whether a run has started and not yet finished.</summary>
    public static bool Running { get; private set; }

    /// <summary>Starts a run on a temporary object. Does nothing while a run is in progress.</summary>
    public static void Run()
    {
        if (Running)
            return;

        Running = true;
        LastReport = null;
        var host = new GameObject("XeSS diagnostics") { hideFlags = HideFlags.HideAndDontSave };
        UnityEngine.Object.DontDestroyOnLoad(host);
        host.AddComponent<Runner>();
    }

    sealed class Runner : MonoBehaviour
    {
        IEnumerator Start()
        {
            var report = new StringBuilder("XeSS diagnostics\n");
            try
            {
                yield return Diagnose(report);
            }
            finally
            {
                LastReport = report.ToString();
                Debug.Log(Tag + LastReport);
                Running = false;
                Destroy(gameObject);
            }
        }

        static IEnumerator Diagnose(StringBuilder report)
        {
            report.AppendLine($"  graphics device: {SystemInfo.graphicsDeviceType}, {SystemInfo.graphicsDeviceName}");
            XessResult versionResult;
            Version sdk;
            try
            {
                versionResult = XessContext.TryGetSdkVersion(out sdk);
            }
            catch (DllNotFoundException e)
            {
                report.AppendLine($"  plugin: not loaded ({e.Message})");
                yield break;
            }
            report.AppendLine($"  plugin renderer: {XessContext.Renderer}");
            report.AppendLine($"  SDK runtime: {versionResult}, version {sdk}");
            if (versionResult != XessResult.Success)
                yield break;

            var size = new Vector2Int(Screen.width, Screen.height);
            const XessInitFlags flags =
                XessInitFlags.LdrInputColor | XessInitFlags.InvertedDepth | XessInitFlags.EnableProfiling;
            XessContext.LogLevel = XessLogLevel.Debug;
            using var context = new XessContext(size, XessQuality.NativeAA, flags);

            float deadline = Time.realtimeSinceStartup + TimeoutSeconds;
            float buildStart = Time.realtimeSinceStartup;
            XessStatus status = context.Status;
            while (status.Stage != XessStage.Failed
                && status.PipelineResult != XessResult.Success
                && Time.realtimeSinceStartup < deadline)
            {
                if (status.Stage == XessStage.Created)
                    context.Poll();
                yield return null;
                status = context.Status;
            }
            report.AppendLine($"  pipelines: {Time.realtimeSinceStartup - buildStart:F2} s");
            if (status.Stage == XessStage.Failed || status.PipelineResult != XessResult.Success)
            {
                report.AppendLine($"  status: {status}");
                yield break;
            }

            context.Initialize();
            while (status.Stage == XessStage.Created && Time.realtimeSinceStartup < deadline)
            {
                yield return null;
                status = context.Status;
            }

            // A folder that does not exist tells a runtime with dumping, which skips the write with a warning, from one
            // without it, which reports the call as not implemented.
            if (status.Stage == XessStage.Initialized)
            {
                string missing = Path.Combine(Application.temporaryCachePath, $"xess-missing-{Guid.NewGuid():N}");
                context.StartDump(missing, 1, XessDumpElements.AllInputs);
                context.Poll();
                while (context.Status.DumpResult == XessResult.NotRun && Time.realtimeSinceStartup < deadline)
                    yield return null;
                status = context.Status;
            }
            report.AppendLine($"  {size.x}x{size.y} Native AA: {status}");
            report.AppendLine($"  profiling records after init: {context.ProfilingRecords().Count}");
        }
    }
}
