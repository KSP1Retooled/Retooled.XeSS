# XeSS

Intel XeSS Super Resolution for Unity's built-in render pipeline on Windows x64.

XeSS records its work into the graphics API's own command list, which C# can't reach. This package includes a small
native plugin, `Retooled.XeSS.Native.dll`, that Unity calls on the render thread. The plugin loads the SDK's `libxess.dll` from
its own folder and makes every SDK call there. `XessContext` is the C# side.

## Graphics APIs

- Direct3D 12: supported on any GPU with Shader Model 6.4.
- Direct3D 11: on Intel Arc, through XeSS's own Direct3D 11 runtime. On other GPUs, the plugin runs XeSS on a
  Direct3D 12 device next to Unity's and copies the textures across, which costs some time each frame.
- Vulkan: the plugin has to hook Unity's Vulkan device creation, so it must load before the device exists. The
  editor loads it on startup. A player that loads it later needs a Vulkan layer, described below. Without the hook
  contexts fail with `ErrorUnsupported`.
- OpenGL, Linux and macOS: XeSS doesn't support them.

### Vulkan in a player

The plugin is marked to load on startup. Where a player loads it before Unity sets up Vulkan, Unity runs the hook as
the editor does. Where it loads later, an application can supply a Vulkan layer of its own that loads
`Retooled.XeSS.Native.dll` when the instance is created and calls its `RetooledVulkanIntercept` export: it takes the
next `vkGetInstanceProcAddr` in the layer's chain and returns one whose `vkCreateInstance` and `vkCreateDevice` add
what XeSS needs, or null if Unity has the hook already. A layer sees every instance in the process and outlives none
of them unless it pins itself, so it should hook only the application's own device.
`XessContext.VulkanDeviceReady` says whether the device has what XeSS needs.

## Use

```csharp
var context = new XessContext(size, XessQuality.NativeAA, XessInitFlags.LdrInputColor, velocityScale);
// Each frame until the pipelines are built:
context.Poll();
if (context.Status.PipelineResult == XessResult.Success)
    context.Initialize();

// Once initialized, record the upscale into a command buffer that runs each frame...
context.RecordExecute(commands);
// ...and queue its parameters once before each run.
context.QueueExecute(parameters);

// When done:
context.Dispose();
```

Calls are queued to the render thread and return at once. `context.Status` shows how far the render thread has got.
The textures in `XessExecuteParams` are native texture pointers. The velocity scale converts the motion vectors into
pixels that point from the current position to the previous one.

To check a machine, call `XessDiagnostics.Run()` and read the report from the log or `XessDiagnostics.LastReport`.

## Debugging

- SDK messages go to the Unity log with an `[XeSS]` prefix. Set `XessContext.LogLevel` before creating a context.
- `XessInitFlags.EnableProfiling` turns on the SDK's GPU timings. Read them with `context.ProfilingRecords()` after a
  poll.
- `context.StartDump` writes the inputs, outputs and history of the next frames to an existing folder. The release
  runtime in SDK 3.0.2 returns `ErrorNotImplemented` for it.

## Building the plugin

Needs CMake, Ninja, LLVM's `clang-cl`, the Windows SDK and the Vulkan SDK's headers. This repository doesn't include
Intel's SDK, so get it first:

```powershell
Native~/fetch-sdk.ps1
Native~/build.ps1
```

`fetch-sdk.ps1` downloads the SDK release, puts its headers in `Native~/xess` and its runtime DLLs in
`Plugins/x86_64`. `build.ps1` copies `Retooled.XeSS.Native.dll` into `Plugins/x86_64`. Close the Unity editor first if it has
loaded the plugin, because it keeps the DLL open.

On GitHub, the Native plug-in workflow builds `Retooled.XeSS.Native.dll` and commits it whenever the plugin's source changes.
Publishing a release packs the package with Intel's DLLs, signs it and adds it to the KSP1Retooled registry.

## Licenses

The SDK runtime DLLs in `Plugins/x86_64` and the headers in `Native~/xess` are Intel's, under the Intel Simplified
Software License in `Native~/xess/LICENSE.txt`, with third-party notices in `Native~/xess/third-party-programs.txt`.
The headers in `Native~/unity` are Unity's, under the Unity Companion License in `Native~/unity/LICENSE.md`.
