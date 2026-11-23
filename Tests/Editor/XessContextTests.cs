using System;
using System.Collections;
using System.Runtime.InteropServices;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.TestTools;

namespace Retooled.XeSS.Tests;

public class XessContextTests
{
    // SDK release 3.0.2 ships the Super Resolution runtime as version 2.0.2.
    [Test]
    public void LoadsTheSdkRuntime()
    {
        RequireRuntime();
        Assert.That(XessContext.TryGetSdkVersion(out Version version), Is.EqualTo(XessResult.Success));
        Assert.That(version, Is.EqualTo(new Version(2, 0, 2)));
    }

    [Test]
    public void StatusMatchesTheNativeLayout() =>
        Assert.That(Marshal.SizeOf<XessNative.Status>(), Is.EqualTo(13 * sizeof(int)));

    // Four pointers and five 32-bit fields, padded to the pointer alignment like the C++ struct.
    [Test]
    public void ExecuteRequestMatchesTheNativeLayout() =>
        Assert.That(Marshal.SizeOf<XessNative.ExecuteRequest>(), Is.EqualTo(56));

    // On the supported graphics APIs the context gets as far as building pipelines. Vulkan needs the plugin to load
    // before the device is created, which the editor does for plugins marked to load on startup.
    [UnityTest]
    public IEnumerator CreatesAContextOrReportsTheApiUnsupported()
    {
        RequireRuntime();
        using var context = new XessContext(new Vector2Int(256, 256), XessQuality.NativeAA, XessInitFlags.None);
        // Batch editors run frames back to back, so the wait is in real time.
        float deadline = Time.realtimeSinceStartup + 10f;
        while (context.Status.Stage == XessStage.Pending && Time.realtimeSinceStartup < deadline)
            yield return null;

        XessStatus status = context.Status;
        if (XessContext.Renderer is GraphicsDeviceType.Direct3D12 or GraphicsDeviceType.Vulkan or GraphicsDeviceType.Direct3D11)
        {
            Assert.That(status.Stage, Is.EqualTo(XessStage.Created), status.ToString());
            Assert.That(status.CreateResult, Is.EqualTo(XessResult.Success));
        }
        else
        {
            Assert.That(status.Stage, Is.EqualTo(XessStage.Failed), status.ToString());
            Assert.That(status.CreateResult, Is.EqualTo(XessResult.ErrorUnsupported));
        }
    }

    // The plugin is only built for Windows, and a checkout of this package may not include Intel's runtime DLLs.
    static void RequireRuntime()
    {
        if (Application.platform != RuntimePlatform.WindowsEditor)
            Assert.Ignore("XeSS only runs on Windows");
        if (XessContext.TryGetSdkVersion(out _) == XessResult.ErrorCantLoadLibrary)
            Assert.Ignore("libxess.dll isn't in Plugins/x86_64");
    }
}
