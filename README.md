# ZZZ DX12 AO

The Windows x64 add-on adds form shading and soft ambient occlusion to the native scene color in Zenless Zone Zero 3.2 before DLSS. It covers the character and other geometry in the game's scene depth.

## Use Detail AO

Replace the previous `zzz_ao_capture.addon64` in the add-on directory of your full-add-on ReShade setup. At 3440×1440 with DLSS Performance, go to **Add-ons → ZZZ DX12 AO** and toggle **Detail AO**. It starts off and can be toggled during the same game session. The ReShade log reports `ZZZ AO submitted Detail AO before upscaling` on the first matching submission.

**Shading View** displays the combined form shading and ambient occlusion in grayscale with expanded contrast. Enable **Detail AO** to use it. Toggle **Shading View** off to see the effect on the game's colors.

Detail AO estimates surface orientation from the game's depth. It averages slopes across neighboring rows and columns to smooth visible facets on curved surfaces. Each smoothing path checks for depth breaks before including more distant samples. A broad camera-relative light gives rounded geometry a gradual light-to-dark transition. Short-range occlusion scans adjacent samples along each direction for a single smooth falloff around thin overlaps. The add-on reuses the native depth view and fullscreen vertex shader for one RGB-only draw at 1720×720 before upscaling and HUD composition. Both modes preserve scene alpha and restore the game's graphics state.

## Build on Windows 11

Install Visual Studio Community 2026 with **Desktop development with C++**, the x64 MSVC toolchain, Windows SDK and CMake 4.2 or newer for the VS 2026 generator. Use a Windows SDK with the x64 DirectX Shader Compiler (`dxc.exe`). The add-on embeds its compiled native pixel shaders. Run these commands in the **x64 Native Tools Command Prompt for VS** from the repository root.

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

CMake downloads and verifies the pinned ReShade 6.7.3 and Dear ImGui 1.92.5 archives for their headers. An offline build can pass `-DRESHADE_INCLUDE_DIR=C:/path/to/reshade/include` and `-DIMGUI_INCLUDE_DIR=C:/path/to/imgui` with the same revisions. If DXC discovery fails, pass `-DDXC_EXECUTABLE="<full path to the SDK's x64 dxc.exe>"` at configure time.

| Output | Path |
| --- | --- |
| Gameplay add-on | `build/Release/zzz_ao_capture.addon64` |
| Native C++ backend | `build/Release/zzz_ao_dx12.lib` |
| Compute shader bytecode | `build/shaders/*.dxil` |

Configure with `-DZZZ_AO_BUILD_COMPUTE=OFF` to build the add-on without the separate XeGTAO backend.

## Optional development trace

Configure a separate build with `-DZZZ_AO_ENABLE_TRACE=ON` to add **Capture DX12 trace** to the add-on panel. When the trace button is available, it arms a short capture that begins when the overlay closes. The ReShade log reports the saved `zzz-ao-<pid>-<capture>.tsv` path in the Windows temporary folder.

The trace records pass metadata and the shader bytecode used to identify the 3.2 depth and scene-color contracts. It also reads the game's preserved pre-AO depth and live depth after later draws, restoring the live depth plane state once the copies are recorded. The paired `character_depth` rows contain the stage name, sample size, and little-endian float32 device depths in hexadecimal.

## Reading the trace

- `texture` declares a session-scoped resource ID, allocation width and height, ReShade format, sample count, mip count, array layers, and usage flags. Viewports can reveal the active internal rectangle when allocations are larger.
- `submit` partitions command-list rows by CPU-observed submission, queue handle and ReShade queue type. `draw`, `dispatch` and `indirect` identify pipeline and shader bytecode fingerprints. Fingerprints use 64-bit FNV-1a and are comparable across captures of the same shaders. `present` identifies the observed presentation interval and back buffer.
- `targets` and `begin_pass` report a depth resource followed by color targets. `depth_clear`, `barrier` and `copy_region` help follow a resource between passes. `root_cbv` reports observed root buffer bindings and offsets without reading buffer contents.
- `table`, `srv` and `uav` report bounded SRV/UAV ranges and known frame-texture bindings at the time of table binding. View formats and register spaces are included. List event rows end with a contiguous repeat count.
- `native_ao_binding` reports a matched shader fingerprint, root layout ID, parameter, descriptor type, table offset, register, register space, count and visibility. `native_ao_shader` records the name, fingerprint, byte count and hexadecimal bytecode of each matched ZZZ 3.2 shader in the submitted work. These rows let a single trace establish the shader's declared inputs and calculations. They are saved locally with the trace.
- `summary` reports observed presentations, submitted lists with no recorded events, and dropped trace rows. `list_truncated` and `descriptor_limit_reached` flag other limits. Capture operates on ReShade callbacks, so previously recorded lists, unproxied work, bindless indexing, and later descriptor rewrites can leave gaps. Cross-queue submission order alone provides insufficient evidence of GPU execution order.

Look for a recurring internal-size depth attachment and scene-color producer. Follow their consumers and the first scaling-sized output. A plausible native AO pass reads depth and possibly normals, writes a separate visibility target, and feeds an ambient lighting pass. Verify those roles from multiple observations, shader evidence, or an authorized GPU capture. The four observed presentations can help compare Frame Generation modes, while presentation callbacks alone cannot classify generated frames or prove the DLSS evaluation boundary.

## Validation and compute backend

Configure with `-DZZZ_AO_TEST_DEPTH_CAPTURE=ON` to run the Windows D3D12 WARP tests. The scene test checks smooth gradients across a sphere and a faceted cylinder, agreement between both views, single shadow falloff around a thin foreground layer, uniform shading on planes, depth discontinuities, scene alpha, and graphics state restoration. The depth test verifies the optional trace's readback. In-game appearance, motion and Frame Generation behavior, and GPU cost require testing on the target system.

The repository also builds a standalone XeGTAO backend with projection-aware depth prefiltering, horizon sampling, and spatial denoising. Its API can write an R8 visibility texture or record a replacement into a caller-provided native visibility mask. The gameplay add-on uses its own shader and pre-DLSS composition path. [Backend API](src/xegtao_dx12.hpp) · [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO)

## Dependencies and licenses

| Dependency | Use and license |
| --- | --- |
| [ReShade v6.7.3](https://github.com/crosire/reshade/tree/4a50d1eddace85734871d91792ff214f13f66c01) | Add-on headers and compatible host. The project license is BSD 3-Clause, and the used headers declare BSD 3-Clause or MIT. See [license notice](third_party/reshade/LICENSE.md). |
| [Dear ImGui 1.92.5](https://github.com/ocornut/imgui/tree/3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c) | MIT. The add-on uses headers for ReShade's overlay API. See [license notice](third_party/imgui/LICENSE.txt). |
| [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO/tree/a5b1686c7ea37788eeb3576b5be47f7c03db532c) | MIT. The two shader headers are vendored under [their license](third_party/xegtao/LICENSE). The source change adds an opt-in final visibility adjustment in the denoiser. |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Build tool from the Windows SDK, NCSA license. DXC binaries are supplied by the installed SDK. |
| [Windows SDK and D3D12](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/) | Visual Studio/Windows SDK installation and system Direct3D 12 runtime under Microsoft's terms. |

The build fetches the ReShade and Dear ImGui header dependencies. The gameplay package contains the add-on, this README, and the dependency license notices.
