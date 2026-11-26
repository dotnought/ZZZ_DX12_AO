# ZZZ DX12 AO

This first milestone provides a Windows x64 ReShade add-on that captures ZZZ's DX12 pass and resource metadata, plus a native D3D12 XeGTAO backend. The ReShade add-on panel can record a read-only trace. The add-on can override the native visibility mask for diagnosis in ZZZ 3.2. The XeGTAO backend can record a replacement into that game's R8 mask format. Live replacement needs verified native inputs and a command-list integration that preserves the game's bindings.

## Build on Windows 11

Install Visual Studio Community 2026 with **Desktop development with C++**, the x64 MSVC toolchain, Windows SDK and CMake 4.2 or newer for the VS 2026 generator. For the compute build, use an SDK with the x64 DirectX Shader Compiler (`dxc.exe`). Run these commands in the **x64 Native Tools Command Prompt for VS** from the repository root.

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

CMake downloads and verifies the pinned ReShade 6.7.3 and Dear ImGui 1.92.5 archives for their headers. An offline build can pass `-DRESHADE_INCLUDE_DIR=C:/path/to/reshade/include` and `-DIMGUI_INCLUDE_DIR=C:/path/to/imgui` with the same revisions. If DXC discovery fails, pass `-DDXC_EXECUTABLE="<full path to the SDK's x64 dxc.exe>"` at configure time.

| Output | Path |
| --- | --- |
| In-game diagnostics | `build/Release/zzz_ao_capture.addon64` |
| Native C++ backend | `build/Release/zzz_ao_dx12.lib` |
| Compute shader bytecode | `build/shaders/*.dxil` |

For a capture-only build, configure with `-DZZZ_AO_BUILD_COMPUTE=OFF` and build `--target zzz_ao_capture`. This requires no DXC.

## ZZZ 3.2 mask test

The supplied traces at 3440×1440 with DLSS Performance show a 1720×720 scene viewport. They contain six scene recordings, two with Frame Generation on and four with it off. The other two observed presentations with Frame Generation on contain only a final image copy. This is consistent with generated presentations, but the trace does not tag them directly.

All six scene recordings contain the same three consecutive pixel shaders producing two distinct 860×360 R8 resources and a 1720×720 R8 resource in that order. The final mask transitions to a readable state just before the suspected lighting shader `16383356142044416029`. The white override visibly reduces shading along curb edges and under furniture. Black darkens the road and storefronts while leaving sampled patches on the player character nearly unchanged. This confirms the native mask's contribution to environment contact shading. Character depth coverage and lighting integration still need investigation. A different compute shader also produces a half-resolution R8 candidate. Both traces show a copied 1720×720 typeless depth-stencil resource viewed as `R32_FLOAT_X8X24_TYPELESS`. Later, internal-size color, depth and two-channel textures are copied before the first 3440×1440 output draw. The exact DLSS evaluation call is outside the observed draw and dispatch events.

To test the native mask, replace the old capture DLL with this build. In the same scene with DLSS Performance at 3440×1440, open the ReShade overlay and go to **Add-ons → ZZZ DX12 AO Capture**. Click **Enable mask test**, then compare the contact shading in the game. The probe writes white after the matching native draw. If you see no change, select **Black mask** in the panel and check again. Use **Disable mask test** to restore the native output. The ReShade log records the first matching submission for each setting. A brief description of what changed is enough for this test. If nothing changed, include whether the log reported an override.

## First in-game capture

Use the documented add-on directory of your existing authorized, full-add-on ReShade setup. The add-on uses ReShade 6.7.3 headers (API 18). Check the host's log for `ZZZ AO DX12 diagnostics loaded` after startup. Keep the capture DLL out of the ReShade shader directory.

1. In DX12 gameplay, use 3440×1440, DLSS Performance and native Frame Generation on. Disable MXAO and its supporting ReShade effects for the capture. Choose a scene where a character is near floor or wall geometry.
2. Click **Capture DX12 trace** in **Add-ons → ZZZ DX12 AO Capture**, then close the overlay. The recorder collects four observed presentations on that swapchain after the overlay closes. Confirm `ZZZ AO capture saved` in the ReShade log and find `zzz-ao-<pid>-<capture>.tsv` in `%TEMP%`.
3. In that same scene and camera position, switch native Frame Generation off and click **Capture DX12 trace** again. Keep DLSS Performance enabled. Restore your settings afterwards.
4. Send both TSV files and indicate which one used Frame Generation. Include the game or ReShade version only if it changed since the previous capture. If the add-on fails to load or save, send the corresponding ReShade log lines.

If the capture shows several plausible scaling chains, the next comparison is a capture with super resolution off. Use the first two traces to decide whether that comparison is needed. This diagnostic build is for pass discovery. Benchmark without the capture add-on loaded because observing descriptor copies introduces CPU overhead.

## Reading the trace

- `texture` declares a session-scoped resource ID, allocation width and height, ReShade format, sample count, mip count, array layers, and usage flags. Viewports can reveal the active internal rectangle when allocations are larger.
- `submit` partitions command-list rows by CPU-observed submission, queue handle and ReShade queue type. `draw`, `dispatch` and `indirect` identify pipeline and shader bytecode fingerprints. Fingerprints use 64-bit FNV-1a and are comparable across captures of the same shaders. `present` identifies the observed presentation interval and back buffer.
- `targets` and `begin_pass` report a depth resource followed by color targets. `depth_clear`, `barrier` and `copy_region` help follow a resource between passes. `root_cbv` reports observed root buffer bindings and offsets without reading buffer contents.
- `table`, `srv` and `uav` report bounded SRV/UAV ranges and known frame-texture bindings at the time of table binding. View formats and register spaces are included. List event rows end with a contiguous repeat count.
- `native_ao_binding` reports a matched shader fingerprint, root layout ID, parameter, descriptor type, table offset, register, register space, count and visibility. `native_ao_shader` records the name, fingerprint, byte count and hexadecimal bytecode of each matched ZZZ 3.2 shader in the submitted work. These rows let a single trace establish the shader's declared inputs and calculations. They are saved locally with the trace.
- `summary` reports observed presentations, submitted lists with no recorded events, and dropped trace rows. `list_truncated` and `descriptor_limit_reached` flag other limits. Capture operates on ReShade callbacks, so previously recorded lists, unproxied work, bindless indexing, and later descriptor rewrites can leave gaps. Cross-queue submission order alone provides insufficient evidence of GPU execution order.

Look for a recurring internal-size depth attachment and scene-color producer. Follow their consumers and the first scaling-sized output. A plausible native AO pass reads depth and possibly normals, writes a separate visibility target, and feeds an ambient lighting pass. Verify those roles from multiple observations, shader evidence, or an authorized GPU capture. The four observed presentations can help compare Frame Generation modes, while presentation callbacks alone cannot classify generated frames or prove the DLSS evaluation boundary.

## Integration and performance

The backend records a depth mip prefilter, one of four XeGTAO horizon passes, and edge-aware spatial denoising to an R8 visibility texture. Enable, intensity, view-space radius, falloff, sample quality, denoising and distance fade are independent. The backend accepts an active internal-resolution rectangle, native device depth conversion, jitter-aware view rays, and optional signed view-space normals. When normals are absent it derives them from depth. A temporal index can advance on each real render frame once a temporal consumer has been verified.

The host must supply an SRV-compatible depth view and confirmed projection, choose a real-frame recording point on the render queue, and manage input states, lifetimes and fence slots. The backend uses dedicated DIRECT command lists on one render queue and tracks transitions for its own textures. The queue serializes shared scratch textures, while each in-flight slot retains its output, descriptors and timestamps. It emits two GPU timestamps and resolves them to readback storage without a CPU wait. `ResidentTextureBytes()` reports committed allocation sizes for its default-heap textures, excluding descriptors, queries and readback. Recreate the backend when the padded render extent changes, after the old work retires. `RecordToNativeMask` copies the active XeGTAO output into the existing single-sample R8 mask target and returns it to render-target state. Timing for that method includes the mask write. The add-on's live recording path still needs to identify the depth and projection bindings and preserve the game's state across the compute dispatch. The backend contract is in [src/xegtao_dx12.hpp](src/xegtao_dx12.hpp).

A confirmed native ambient/indirect lighting consumer is the preferred AO composition point. If that path is inaccessible, validate an internal-resolution scene-color composition point before DLSS Super Resolution and HUD rendering. NVIDIA documents the separate scaling input, depth, motion vectors and HUD-less inputs, so the observer needs to establish how ZZZ uses them. HoYoverse documents the graphics settings without publishing the underlying resource bindings. [DLSS Super Resolution integration](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS.md) · [DLSS Frame Generation integration](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md) · [ZZZ PC visual experience FAQ](https://zenless.hoyoverse.com/en-us/news/165408?catchSpider=1&lang=en-us&page=news)

The ZZZ 3.2 override validates the native environment visibility mask. Replacing the calculation at that mask lets the game consume the result through its existing lighting path. How much of the character shading uses that path is still open. For replacement, XeGTAO's three-stage compute path, native-depth input, optional normals, compact visibility output and spatial denoiser are a good fit for internal-resolution AO feeding DLSS. CACAO offers adaptive and downsampled operation with additional pass/resource configurations. Both are MIT-licensed. The initial backend uses FP32 depth for the reconstructed-normal path. A measured FP16 path with confirmed native normals is a later optimization. The ReShade depth, frame-effect and shader replacement examples and RenoDX demonstrate relevant observation and in-frame add-on patterns. [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO) · [AMD CACAO](https://gpuopen.com/fidelityfx-cacao/) · [ReShade examples](https://github.com/crosire/reshade/tree/v6.7.3/examples) · [RenoDX](https://github.com/clshortfuse/renodx)

The first composition build still needs game validation for HUD isolation, stable motion, Frame Generation parity, resize and alt-tab behavior, and scene transitions. In-game settings UI, debug visualization and AO resolution scaling are pending that integration. Measure AO plus composition GPU time, report median and 95th percentile, and compare frame time against the unmodified game and visually matched iMMERSE MXAO at the same settings. Treat 0.5, 1, 2 and 3 ms for Low through Ultra as targets for the RTX 4060 Ti.

## Dependencies and licenses

| Dependency | Use and license |
| --- | --- |
| [ReShade v6.7.3](https://github.com/crosire/reshade/tree/4a50d1eddace85734871d91792ff214f13f66c01) | Add-on headers and compatible host. The project license is BSD 3-Clause, and the used headers declare BSD 3-Clause or MIT. See [license notice](third_party/reshade/LICENSE.md). |
| [Dear ImGui 1.92.5](https://github.com/ocornut/imgui/tree/3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c) | MIT. The add-on uses headers for ReShade's overlay API. See [license notice](third_party/imgui/LICENSE.txt). |
| [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO/tree/a5b1686c7ea37788eeb3576b5be47f7c03db532c) | MIT. The two shader headers are vendored under [their license](third_party/xegtao/LICENSE). The source change adds an opt-in final visibility adjustment in the denoiser. |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Build tool from the Windows SDK, NCSA license. DXC binaries are supplied by the installed SDK. |
| [Windows SDK and D3D12](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/) | Visual Studio/Windows SDK installation and system Direct3D 12 runtime under Microsoft's terms. |

CACAO, RenoDX, NVIDIA Streamline and the game are references for this milestone. The build fetches only the ReShade and Dear ImGui header dependencies and ships no game or NVIDIA binaries.
