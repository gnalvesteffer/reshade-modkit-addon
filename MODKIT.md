# ReShade Mod Kit Add-on

A standalone ReShade add-on with an in-game graphics inspector and live mod controls integrated into the **Mod Kit** overlay tab. The workflow is inspired by RenderDoc's event browser and pipeline viewer: select a frame action, inspect its bound shaders and resources, then change the running game's graphics using resource replacements and draw rules.

This implementation uses ReShade's existing Direct3D 9/10/11/12, OpenGL and Vulkan hooks. It does not guarantee support for every game. It is a development implementation that needs validation in real games before distribution as a stable injector.

## Build and install

Install a compatible **full add-on support** ReShade build (SDK API 20 / ImGui 1.92.5), then copy `ModKit.addon64` beside its proxy DLL. This project builds only the add-on; it does not replace ReShade. Open **Home → Mod Kit**. Remove or disable any built-in fork version before loading this add-on, to avoid running two inspectors.

Windows, from a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

Linux with MinGW:

```sh
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw --parallel
```

All build dependencies are vendored with their licenses and pinned revisions in `vendor/versions.json`. No injector build, fork-specific SDK events, or submodule downloads are needed.

## Inspector workflow

1. Select **Capture next frame**, or enable **Live capture**. **Freeze** keeps the displayed frame while gameplay continues.
2. Select an action in **Frame / pipeline**. The detail pane shows bound shader IDs, textures, buffers, render targets, viewports, scissors and dynamic state. Search by shader/resource ID, action or region label.
3. Export a shader, texture or buffer; or create a replacement from a captured resource. Each game's files live under `reshade-modkit/<executable-name>/` beside the ReShade configuration.
4. Edit replacement files externally, or use **Edit source** and **Editor / rules**. GLSL source can be edited directly. DXBC Shader Model 4/5 HLSL replacements can be compiled in the panel. DXIL/Shader Model 6 requires an external compiler such as DXC; SPIR-V requires an external SPIR-V compiler. Keep the original stage, entry point and resource/input/output interface compatible.
5. Leave **Watch replacement files** enabled to reload changes about every 500 ms, or select **Reload mods**. Compilation and GPU allocation happen when a replacement is first needed, and can briefly stall rendering.
6. Toggle **Suppress draws using this shader** to identify the geometry or effect a shader draws. Toggle the resource's replacement checkbox, or **Enable mods**, to compare the original and modified graphics.

The shader browser includes bytecode disassembly for formats accepted by Windows' D3D disassembler and GLSL source. The texture browser previews an owned snapshot or replacement. **Capture CPU uploads** in **Session** is off by default; enable it to collect bytes and content IDs from newly observed uploads. Existing GPU-only resources remain session IDs until their contents are captured. The buffer browser shows the first 4 KiB as hexadecimal. **Snapshot GPU contents** reads buffers and base texture mips on Direct3D 9/10/11 and OpenGL, where ReShade abstracts synchronization implicitly. This explicit readback waits for the GPU and can stall a frame.

Direct3D 12 and Vulkan previews use CPU uploads observed during creation, updates, mapping and staging-buffer-to-texture copies. GPU-only readback on these APIs needs additional per-subresource state and queue ownership tracking and is not implemented. Resources without captured bytes remain visible and can be replaced by session ID when their binding path is supported.

## Replacement files

```text
reshade-modkit/GameExecutable/
  shaders/<16-digit-shader-hash>.cso   # Direct3D bytecode
  shaders/<16-digit-shader-hash>.spv   # SPIR-V
  shaders/<16-digit-shader-hash>.glsl  # OpenGL source
  textures/<resource-id>.png
  textures/<resource-id>.bin
  buffers/<resource-id>.bin
  rules.ini
  captures/                          # Original exports and frame TSV files
```

Shader hashes are 64-bit FNV-1a hashes of original code. Resource content hashes include tightly packed original buffer/base-mip data and dimensions/format/stride. Texture hashes exclude row padding. A resource without CPU contents uses `session-<serial>` and will need to be identified again after restarting the game. These hashes are Mod Kit IDs, distinct from the CRC32 names used by upstream's example add-ons.

Texture replacement supports sampled 2D, single-layer, single-sample textures. PNG must match the original dimensions and requires RGBA8/BGRA8 format; all original mip levels are rebuilt with a box filter in encoded color space. Native `.bin` supports other formats including block compression and contains **every** mip level in increasing order, each tightly packed using ReShade's format pitch calculations. PNG takes precedence if both files exist. Texture exports with the `.mip0.bin` extension contain only the base mip and cannot replace a multi-mip texture directly. Buffer files must exactly match the original buffer size.

Replacement resources are separate allocations. Original resource contents and game writes are preserved. The mod binds replacement shaders/resources for one intercepted action, replays that action, then restores the application's state. Texture views preserve the game's original view description. Writable render targets, depth buffers, storage buffers/UAVs and stream output resources are inspectable but not automatically substituted.

## Draw rules

Edit `rules.ini` externally or in the panel. Section names are descriptive. All specified filters must match; matching rules are applied in file order. `shader` matches an original bound shader hash. Omitted filters match any value. Zero is a valid filter or replacement value.

```ini
[hide a particle pass]
kind=indexed
shader=0123456789ABCDEF
count=240
action=skip

[change an instanced draw]
kind=draw
shader=FEDCBA9876543210
instances=8
action=modify
replace_instances=4
```

Kinds: `any`, `draw`, `indexed`, `dispatch`, `mesh`, `indirect`. Filters: `shader`, `count`, `instances`. Actions: `skip`, `modify`. Changes: `replace_count`, `replace_instances`. For dispatch/mesh, count and instances correspond to X and Y; Z is preserved. For indirect commands only suppression and resource/shader replacement are supported; the argument buffer is not rewritten by numeric rules. Indirect events with an unknown command type, including D3D12 ExecuteIndirect, support suppression and are otherwise passed through because the generic event does not expose the native signature or count buffer needed to replay them correctly. Changing draw sizes requires the mod author to ensure the bound geometry/buffers cover the new range. Invalid rules are rejected as a whole and the previous valid rules remain active.

Panel suppression switches are session controls; use `rules.ini` to persist a mod.

## API coverage and current boundaries

| Feature | Coverage |
| --- | --- |
| Draw/indexed/compute/mesh interception | ReShade events on the APIs that support each command |
| Shader inspection/export | Shader code supplied through observed pipeline creation |
| Shader replacement | Rebuildable raster/compute pipelines; old pipeline retained if creation fails |
| Texture/buffer substitution | Direct pushed descriptors, vertex/index buffers and supported descriptor tables |
| D3D12/Vulkan table substitution | Fully observed tables with at most 255 descriptors; unbounded, partially bound and static-sampler ranges are passed through |
| Frame capture | Direct actions on implicit APIs; retained command-buffer actions in submission order on D3D12/Vulkan, including reused command buffers |
| Region labels | Native marker hooks from the former fork are not included; public SDK events do not expose them |
| GPU timing | Opt-in D3D12 direct draw/indexed/dispatch/mesh actions; asynchronous per-submission timestamp readbacks |
| Suppression comparison | Four alternating enabled/suppressed phases using CPU-observed present intervals |
| Shader editing | GLSL; HLSL compilation for DXBC Shader Model 4/5; external compilation for DXIL/SPIR-V |
| Readback | Implicit APIs; CPU upload snapshots for explicit APIs |

This is an inspector of intercepted command metadata and a graphics modding tool. It does not replay a frame offline, debug individual shader invocations, expose GPU performance counters, recover missing source code, or guarantee compatibility with injected overlays or games that restrict injection. Ray tracing pipeline replacement is disabled because it also requires rewriting shader binding tables. Native PIX and other marker payloads are not decoded by this add-on.

On explicit APIs, replacements affect newly recorded commands. A command buffer already recorded keeps its existing bindings until the game records it again; this also applies when disabling a mod. When capture, profiling, and comparisons are off, new commands do not retain detailed draw records. Capture includes cached commands only if their metadata was recorded while one of those modes was active. A first capture can omit cached commands recorded before capture began; wait for the game to record them again. D3D11 deferred contexts currently expose recording-time actions rather than reconstructed execution order. The first implementation records draws/dispatches; copies/clears and state-setting API calls are not individual event-browser rows.

GPU replacements and descriptor snapshots are retained until device teardown because an application can resubmit an old command buffer after any number of frames. Limits: 256 MiB of CPU resource/shader snapshots, 64 MiB of retained command metadata, 512 MiB of estimated replacement/preview data, 1024 rebuilt pipelines, 4096 descriptor snapshots, 16384 retained resource views and 8192 actions per displayed frame. Exhausting a limit leaves the current game resource or last working replacement active. Restart the game to release retained GPU versions. Long editing sessions and heavily changing descriptor tables can reach these limits.

OpenGL program replacements copy matching uniforms by name, including sampler units, matrix/vector values, uniform block bindings and shader storage block bindings. A changed or unsupported uniform type, relocated vertex attribute, or transform feedback declaration retains the original program for that action. Fragment output locations are checked when program interface queries are available; on older GL versions, preserve their original locations explicitly in source. Separate shader program pipelines and shader subroutine state are not supported by this path. D3D11 shaders using dynamic class linkage can be inspected and exported, but replacement is disabled because the generic binding API does not expose class instances.

## Performance and profiling

**Live capture**, **GPU timing**, and **Capture CPU uploads** are off by default. Idle draws bypass record construction; suppression uses cached shader IDs. When no replacement files exist, draws do not scan replacement resources or descriptor tables. Repeated pipeline, descriptor and table binds update existing state without rebuilding vectors. Tracking binding state and ReShade interception still have a CPU cost; actual BeamNG performance improvements need a game run.

For GPU timings, open **Profiler**, enable **GPU timing**, then **Capture timed frame**. The checkbox initially requests a frame; if its commands were recorded before timing was enabled, capture again. **Frame / pipeline** shows `GPU ms`, offers sorting by cost, and exports timing values/status in TSV. The Profiler lists shader totals, averages, maxima and timed action counts. Each shader is credited with the entire draw using it, so totals across shader stages overlap. For profiling with fewer metadata costs, disable **Capture resource bindings**.

The add-on lazily installs its own D3D12 queue submission hook using MinHook. It injects two GPU timestamps around a replayed draw/dispatch, resolves them after the game's native submission into a separate readback buffer, and polls one fence per submission on later frames. Normal collection does not flush or wait for GPU idle. D3D12 `ExecuteIndirect`, bundles, and other APIs are currently untimed. Timestamp intervals are estimates influenced by pipeline overlap, GPU scheduling, and instrumentation; they are not isolated hardware execution costs.

Disabling timing stops inserting timestamps and collecting new measurements. Cached command lists still contain previously inserted timestamps until the game records them again. The add-on continues lightweight lifetime fence tracking for these commands; it cannot remove commands already recorded by the game. Query slots shared by a cached list migrating between queues require a GPU dependency to keep them safe, which can affect overlap during profiling. Duplicate execution of the same timestamp slots within a submission has no reported timings. A small forwarding hook remains installed after the first use and is removed at add-on shutdown. Live unloading while the game is rendering is not supported.

The timestamp pool is bounded to 128 pages of 256 draws; up to 32 submission readbacks and 8192 timed actions per submission are retained. Exhaustion shows missing/dropped measurements and passes through rendering. Failed readbacks retain affected slots until device teardown so in-flight GPU resources stay alive. Queue teardown may wait for profiling work to complete; regular frame collection never waits for GPU idle.

For practical savings, select a shader and choose **Measure enabled / suppressed** (or its shader detail button). Disable GPU timing first. The comparison alternates enabled/suppressed/enabled/suppressed, discards 30 warmup frames per phase, and collects 60–600 samples per phase (120 by default). It checks the target shader's actual recorded suppression state before counting a frame, restores its original setting on completion/cancellation, and times out if the target draws never appear or cannot change state. Frames longer than 250 ms restart warmup. Configuration reloads cancel the comparison.

Results are CPU-observed present intervals with means, standard deviations, and their difference. They include CPU work, frame caps, VSync and scheduling; they are not a GPU frame-time measurement. Keep the camera, scene, capture state and other mods constant. Disabling a shader changes downstream rendering and overlapping work, so the difference measures the net benefit for that scene. A CPU bottleneck or frame cap can hide GPU savings. The original suppression state is applied to newly recorded commands after the comparison.

## Build coverage

A local MinGW cross build and Windows CI build compile the add-on. This does not establish game compatibility or profiling accuracy under Proton; those need a real game run. No runtime checks or benchmark claims are implied by successful compilation.

## References

The UI workflow and marker semantics were informed by [RenderDoc's event browser](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/event_browser.rst), [pipeline state viewer](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/pipeline_state.rst), and [annotation documentation](https://github.com/baldurk/renderdoc/blob/v1.x/docs/how/how_annotate_capture.rst). RenderDoc was inspected as a source reference; its capture/replay engine is not linked into this add-on. [Microsoft PixEvents](https://github.com/microsoft/PixEvents) provides the public PIX event runtime, and [Intel GPA Framework](https://intel.github.io/gpasdk-doc/) documents Intel's framework. Neither inspector is a dependency of Mod Kit. Existing ReShade and third-party licenses remain in place.
