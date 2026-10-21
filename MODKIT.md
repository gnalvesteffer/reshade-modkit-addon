# ReShade Mod Kit Add-on

A standalone ReShade add-on with an in-game graphics inspector and live mod controls integrated into the **Mod Kit** overlay tab. The workflow is inspired by RenderDoc's event browser and pipeline viewer: select a frame action, inspect its bound shaders and resources, then change the running game's graphics using resource replacements and draw rules.

This implementation uses ReShade's existing Direct3D 9/10/11/12, OpenGL and Vulkan hooks. It does not guarantee support for every game. It is a development implementation that needs validation in real games before distribution as a stable injector.

## Build and install

Install a compatible **full add-on support** ReShade build (SDK API 20 / ImGui 1.92.5), then copy `ModKit.addon64` and `ModKit-tools/` beside its proxy DLL. This project builds only the add-on; it does not replace ReShade. Open **Home → Mod Kit**. Remove or disable any built-in fork version before loading this add-on, to avoid running two inspectors.

Windows, from a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
python scripts/fetch-dxc.py --destination build/Release
```

Linux with MinGW:

```sh
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw --parallel
python3 scripts/fetch-dxc.py --destination build-mingw
```

Build headers and sources are vendored with their licenses and pinned revisions in `vendor/versions.json`. The packaging script downloads a pinned Microsoft DXC release and checks its SHA256 before extracting the disassembler and its licenses. No injector build, fork-specific SDK events, or submodule downloads are needed.

## Inspector workflow

1. Select **Capture next frame**, or enable **Live capture**. **Freeze** keeps the displayed frame while gameplay continues.
2. Select an action in **Frame / pipeline**. The detail pane shows bound shader IDs, textures, buffers, render targets, viewports, scissors and dynamic state. Search by shader label, shader/resource ID or action.
3. Export a shader, texture or buffer; or create a replacement from a captured resource. Each game's files live under `reshade-modkit/<executable-name>/` beside the ReShade configuration.
4. For a shader, choose **Open replacement editor** (Direct3D) or **Edit GLSL source**. This switches to **Editor / rules** and opens an existing replacement or new source. Original HLSL is not reconstructed from compiled bytecode. The Direct3D editor displays original reflected inputs, outputs, resource registers and disassembly; supported pixel shaders get a generated magenta-output starter with the original output semantics. This starter identifies a shader's contribution and deliberately replaces its shading. Edit the color or write compatible HLSL, then **Compile and apply**. Shader Model 4/5 compiles with D3DCompiler and Shader Model 6 with bundled DXC and its validator. **Import source...** opens user-supplied HLSL/GLSL; **Import compiled replacement...** installs externally compiled Direct3D/SPIR-V bytecode. SPIR-V still needs an external authoring/compiler tool. Keep the original stage, entry point and resource/input/output interface compatible. The embedded editor accepts text smaller than 64 KiB.
5. Leave **Watch replacement files** enabled to reload changes about every 500 ms, or select **Reload mods**. Compilation and GPU allocation happen when a replacement is first needed, and can briefly stall rendering.
6. Toggle **Suppress draws using this shader** to identify the geometry or effect a shader draws. Toggle the resource's replacement checkbox, or **Enable mods**, to compare the original and modified graphics.

**Save source** retains HLSL on disk without compiling it. **Compile and apply** saves source and publishes compiled bytecode only after compilation succeeds. Compiler warnings/errors remain visible beside the editor; a failed compilation preserves the previous compiled replacement. **Generate pixel shader starter** can replace an older comment-only template with working diagnostic HLSL where reflection is supported. Other stages need user-authored compatible source. **Open externally** saves and opens the file with the desktop's associated application; **Reload file** refreshes the editor. Compilation reloads external changes when there are no unsaved in-game edits, and refuses to overwrite a source file changed both externally and in the editor. Paths can be copied if desktop application launch is unavailable under a compatibility layer.

**Compiler settings: includes and defines** accepts one include directory per line and one `NAME` or `NAME=VALUE` definition per line. Source imports add the original file's directory so its local headers can be found. Add any game/source root used by root-relative includes and the defines required for that shader variant. Entry point, target, include paths and definitions are saved in `shaders/<shader-hash>.compile.ini` alongside the source. Some games ship original source separately from the compiled shaders; matching that source and its build settings remains the mod author's responsibility.

Texture detail panels provide **Edit PNG externally** for captured RGBA8/BGRA8 images and **Import PNG...** for matching-dimension replacements. The external edit action creates a replacement from the snapshot if needed and opens the existing file without overwriting edits. File watching reloads saved images; **Reload mods** is available when watching is disabled. Compressed and other non-RGBA8/BGRA8 formats currently use **Import native replacement...**, which requires every mip in tightly packed original format order. A `.mip0.bin` capture alone is insufficient for a multi-mip replacement. Writable resources and unsupported layouts show why replacement is unavailable.

The shader browser includes DXBC disassembly, DXIL assembly using the bundled Microsoft DXC library, and GLSL source. Open **Shader disassembly/source** to read it; **Export disassembly** saves text in `captures/`. Original HLSL is not reconstructed from bytecode. SPIR-V still requires an external disassembler. Disassembly is cached per shader (128 entries / 64 MiB total, 8 MiB per entry), so viewing several shaders does not repeatedly disassemble them. DXC loads only when requested, from the add-on's own `ModKit-tools/` folder.

The texture browser previews an owned snapshot or replacement. **Capture CPU uploads** in **Session** is off by default; enable it to collect bytes and content IDs from newly observed uploads. Existing GPU-only resources remain session IDs until their contents are captured. The buffer browser shows the first 4 KiB as hexadecimal. **Snapshot GPU contents** reads buffers and base texture mips on Direct3D 9/10/11 and OpenGL, where ReShade abstracts synchronization implicitly. This explicit readback waits for the GPU and can stall a frame.

Direct3D 12 adds on-demand GPU-only texture snapshots at observed legacy transitions; see **Texture previews** below. Vulkan and unsupported D3D12 resources use CPU uploads observed during creation, updates, mapping and staging-buffer-to-texture copies. Resources without captured bytes remain visible and can be replaced by session ID when their binding path is supported.

## Persistent shader labels

Select a shader in the shader browser or a captured draw, enter a **Label** such as `Sky shader` or `Car metal shader`, then click **Save labels**. Label edits stay in memory until saved. Clearing the label and saving removes it. Saved labels load on the next game run and appear in the shader browser, capture search, profiler summaries and exported frame TSV files.

Labels are stored per game in `shader-labels.tsv`, using original shader bytecode hashes. Each non-comment line contains a 16-digit hexadecimal ID, a tab, and a UTF-8 label. They can be shared or edited while the game is closed. A game update that changes shader bytecode changes its hash, so the new shader may need a new label. Labels do not enable shader replacement or suppression.

Games can also emit PIX render-pass markers and debug object names. Those describe engine events or objects and may be absent in retail builds. Receiving them requires native API interception; the public ReShade SDK does not expose marker callbacks. Native marker capture is not implemented in this release.

## Run ReShade effects before the UI

Initial support is **experimental D3D12**. Capture a frame in **Frame / pipeline**, select the draw that starts the UI layer, and click **Run ReShade effects before this draw**. ReShade runs on that draw's output before the draw executes, so the UI can then be composited above the processed image. Both the swap-chain back buffer and off-screen color textures are supported when the output is one single-sample 2D texture with the same dimensions as ReShade's output. The add-on stores the draw kind, complete set of original shader IDs, and output target type; for off-screen targets it also matches width, height, and format. **Match selected draw count** adds the selected vertex/index count as a filter; it is off initially because UI batches can change size between frames. If multiple off-screen targets have identical dimensions and format and use the same shaders, the first matching draw in command order is selected.

At the match, the add-on calls the public `effect_runtime::render_effects()` API with views for the selected output, restores observed graphics and compute bindings, and ReShade skips its normal presentation-time effects pass. When the execution point is enabled but no supported draw matches in a frame, the add-on skips the normal pass for that frame; the Mod Kit status reports the miss. Effects are therefore absent for a frame until a supported draw matches. **Enable early ReShade effects** disables the execution point; **Clear execution point** removes the saved match. Saved enabled state is a user preference; games with no saved point use normal ReShade timing.

Supported matches use one single-sampled color target at ReShade's output dimensions, on a direct command list outside native render passes and active queries. The command list must have previously submitted on the runtime's graphics queue, and must have been recorded since the execution point was enabled or changed, with an observed root signature, PSO, shader-visible descriptor heap, viewports, scissors and primitive topology. State hooks are installed only when early effects are enabled: they preserve native descriptor heap bindings, including bindless rendering, and the full floating-point blend factor. SDK callbacks track root constants only while enabled. A forwarding hook remains after first use, while metadata copying stops when the feature is disabled.

This does not automatically find the UI or exclude UI already drawn. It does not support off-screen targets at a different size from ReShade's output, multiple color targets, multisampled targets, Vulkan render-pass injection, indirect draws, bundles or arbitrary queue/parallel recording order. Use a draw on the runtime's regular graphics queue, recorded in frame order. Reused command lists containing an injected pass are detected at submission: further insertion is disabled and normal presentation processing is suppressed for those lists to avoid applying effects twice. Already recorded commands cannot be removed; they retain old effect bindings and uniforms. Re-record them or restart the game before reloading effects or changing the preset. Runtime compatibility and visual results still need a game run.

## Replacement files

```text
reshade-modkit/GameExecutable/
  shaders/<16-digit-shader-hash>.cso   # Direct3D bytecode
  shaders/<16-digit-shader-hash>.spv   # SPIR-V
  shaders/<16-digit-shader-hash>.glsl  # OpenGL source
  shaders/<16-digit-shader-hash>.hlsl  # Replacement HLSL source
  shaders/<16-digit-shader-hash>.compile.ini # HLSL compiler settings
  textures/<resource-id>.png
  textures/<resource-id>.bin
  buffers/<resource-id>.bin
  rules.ini
  shader-labels.tsv                   # Saved user labels keyed by shader hash
  effect-point.ini                    # Saved early ReShade draw match
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

**Reset all suppression** unticks all shader suppression controls, including those outside the current filter, and cancels any suppression comparison before clearing the controls. It preserves replacements and saved rules. A skip rule in `rules.ini` can still suppress a draw. On D3D12/Vulkan, commands whose draws were already omitted must be recorded again by the game before those draws return.

## Texture previews

Selecting a 2D texture displays a base mip image from a retained CPU snapshot or replacement. Normalized, floating-point and BC compressed color formats use the GPU sampler to display their contents. Integer, depth/stencil, multisampled, planar, array and 3D visualization needs further conversion/view selection support. HDR values are shown through the normal overlay shader, without exposure controls or tone mapping.

For D3D12 GPU-only textures, selection requests an asynchronous snapshot. **Snapshot GPU contents** refreshes it; **Cancel preview request** stops a waiting request. Only the requested texture is copied. The add-on waits for a complete legacy transition of mip 0 on a direct command list previously observed on the runtime's graphics queue, outside render passes and queries. It uses the native barrier's exact final state, copies mip 0 into a readback buffer, then restores that state. Split transitions and ambiguous aliasing batches are skipped. A submission fence must complete before the buffer is mapped; readback collection does not flush or wait for GPU idle. Creating the displayed preview from those bytes uses ReShade's resource upload API, which can briefly stall while uploading. Snapshot contents can also be exported or used to seed a replacement.

This captures the next qualifying transition, rather than the exact captured draw's contents. A static texture with no later transition can remain pending; a scene reload may update or replace it, requiring reselection. Enhanced barriers, copy/compute queue capture and Vulkan GPU-only readback are not covered yet. CPU upload snapshots and replacements remain available on those paths. Request failures and waiting status are shown beside the preview.

Native preview hooks are installed on demand. The original command list retains its injected copy until the game records it again, so replaying a cached list can still incur a copy after the preview is frozen. Each snapshot uses a fence; cross-queue reuse adds a GPU dependency. Readback buffers referenced by recorded lists stay alive until device teardown, bounded by 128 snapshots, 64 MiB per snapshot and the shared 512 MiB retained allocation limit. Unrecorded cancelled requests release their allocation. Preview sampling and readback still need validation in a game.

## API coverage and current boundaries

| Feature | Coverage |
| --- | --- |
| Draw/indexed/compute/mesh interception | ReShade events on the APIs that support each command |
| Shader inspection/export | Shader code supplied through observed pipeline creation |
| Saved shader labels | Per-game bytecode hash labels, shown in search, profiling and exports |
| Shader disassembly | DXBC via D3DCompiler; DXIL via bundled DXC; GLSL source |
| ReShade execution point | Experimental D3D12 before a matching draw to the current back buffer |
| Shader replacement | Rebuildable raster/compute pipelines; old pipeline retained if creation fails |
| Texture/buffer substitution | Direct pushed descriptors, vertex/index buffers and supported descriptor tables |
| D3D12/Vulkan table substitution | Fully observed tables with at most 255 descriptors; unbounded, partially bound and static-sampler ranges are passed through |
| Frame capture | Direct actions on implicit APIs; retained command-buffer actions in submission order on D3D12/Vulkan, including reused command buffers |
| Region labels | Native marker hooks from the former fork are not included; public SDK events do not expose them |
| GPU timing | Opt-in D3D12 direct draw/indexed/dispatch/mesh actions; asynchronous per-submission timestamp readbacks |
| Suppression comparison | Four alternating enabled/suppressed phases using CPU-observed present intervals |
| Shader editing | GLSL; reflected pixel starters and HLSL compilation for DXBC Shader Model 4/5 and DXIL Shader Model 6; external compilation for SPIR-V |
| Readback | Implicit APIs; on-demand D3D12 color mip snapshots at observed legacy transitions; CPU uploads on other explicit paths |

This is an inspector of intercepted command metadata and a graphics modding tool. It does not replay a frame offline, debug individual shader invocations, expose GPU performance counters, recover missing source code, or guarantee compatibility with injected overlays or games that restrict injection. Ray tracing pipeline replacement is disabled because it also requires rewriting shader binding tables. Native PIX and other marker payloads are not decoded by this add-on.

On explicit APIs, replacements affect newly recorded commands. A command buffer already recorded keeps its existing bindings until the game records it again; this also applies when disabling a mod. When capture, profiling, and comparisons are off, new commands do not retain detailed draw records. Capture includes cached commands only if their metadata was recorded while one of those modes was active. A first capture can omit cached commands recorded before capture began; wait for the game to record them again. D3D11 deferred contexts currently expose recording-time actions rather than reconstructed execution order. The first implementation records draws/dispatches; copies/clears and state-setting API calls are not individual event-browser rows.

GPU replacements and descriptor snapshots are retained until device teardown because an application can resubmit an old command buffer after any number of frames. Limits: 256 MiB of CPU resource/shader snapshots, 64 MiB of retained command metadata, 512 MiB of estimated replacement/preview data, 1024 rebuilt pipelines, 4096 descriptor snapshots, 16384 retained resource views and 8192 actions per displayed frame. Exhausting a limit leaves the current game resource or last working replacement active. Restart the game to release retained GPU versions. Long editing sessions and heavily changing descriptor tables can reach these limits.

OpenGL program replacements copy matching uniforms by name, including sampler units, matrix/vector values, uniform block bindings and shader storage block bindings. A changed or unsupported uniform type, relocated vertex attribute, or transform feedback declaration retains the original program for that action. Fragment output locations are checked when program interface queries are available; on older GL versions, preserve their original locations explicitly in source. Separate shader program pipelines and shader subroutine state are not supported by this path. D3D11 shaders using dynamic class linkage can be inspected and exported, but replacement is disabled because the generic binding API does not expose class instances.

## Performance and profiling

**Live capture**, **GPU timing**, and **Capture CPU uploads** are off by default. Idle draws bypass record construction; suppression uses cached shader IDs. When no replacement files exist, draws do not scan replacement resources or descriptor tables. Repeated pipeline, descriptor and table binds update existing state without rebuilding vectors. Tracking binding state and ReShade interception still have a CPU cost; actual BeamNG performance improvements need a game run.

For GPU timings, open **Profiler**, enable **GPU timing**, then **Capture timed frame**. The checkbox initially requests a frame; if its commands were recorded before timing was enabled, capture again. **Frame / pipeline** shows `GPU ms`, offers sorting by cost, and exports timing values/status in TSV. The Profiler lists shader totals, averages, maxima and timed action counts. Live timing displays the newest completed measured frame from a queue of at most four frames, so results can lag gameplay. Single frame captures display pending results until ready. Each shader is credited with the entire draw using it, so totals across shader stages overlap. For profiling with fewer metadata costs, disable **Capture resource bindings**. Until a frame has been captured, both tabs show a hint for recording one (and note when capture is already in progress). While GPU timing is enabled and newer frames still await timestamp readback, the Profiler states which frame is displayed and how many newer ones are waiting; the Frame / pipeline tab keeps its "Waiting for GPU measurements" line.

The add-on lazily installs its own D3D12 queue submission hook using MinHook. It injects two GPU timestamps around a replayed draw/dispatch, resolves them after the game's native submission into a separate readback buffer, and polls one fence per submission on later frames. Normal collection does not flush or wait for GPU idle. D3D12 `ExecuteIndirect`, bundles, and other APIs are currently untimed. Timestamp intervals are estimates influenced by pipeline overlap, GPU scheduling, and instrumentation; they are not isolated hardware execution costs.

Disabling timing stops inserting timestamps and collecting new measurements. Cached command lists still contain previously inserted timestamps until the game records them again. The add-on continues lightweight lifetime fence tracking for these commands; it cannot remove commands already recorded by the game. Query slots shared by a cached list migrating between queues require a GPU dependency to keep them safe, which can affect overlap during profiling. Duplicate execution of the same timestamp slots within a submission has no reported timings. A small forwarding hook remains installed after the first use and is removed at add-on shutdown. Live unloading while the game is rendering is not supported.

The timestamp pool is bounded to 128 pages of 256 draws; up to 32 submission readbacks and 8192 timed actions per submission are retained. Exhaustion shows missing/dropped measurements and passes through rendering. Failed readbacks retain affected slots until device teardown so in-flight GPU resources stay alive. Queue teardown may wait for profiling work to complete; regular frame collection never waits for GPU idle.

For practical savings, select a shader and choose **Measure enabled / suppressed** (or its shader detail button). Disable GPU timing first. The comparison alternates enabled/suppressed/enabled/suppressed, discards 30 warmup frames per phase, and collects 60–600 samples per phase (120 by default). It checks the target shader's actual recorded suppression state before counting a frame, restores its original setting on completion/cancellation, and times out if the target draws never appear or cannot change state. Frames longer than 250 ms restart warmup. Configuration reloads cancel the comparison.

Results are CPU-observed present intervals with means, standard deviations, and their difference. They include CPU work, frame caps, VSync and scheduling; they are not a GPU frame-time measurement. Keep the camera, scene, capture state and other mods constant. Disabling a shader changes downstream rendering and overlapping work, so the difference measures the net benefit for that scene. A CPU bottleneck or frame cap can hide GPU savings. The original suppression state is applied to newly recorded commands after the comparison.

## Build coverage

A local MinGW cross build and Windows CI build compile the add-on. CI runs portable profile parsing, package/checksum checks and Windows checks for saved labels, execution points, texture snapshot guards and shader editing. Shader checks compile DXBC/DXIL originals and generated pixel replacements, verify output signatures, exercise nested includes and defines, and check failed compilation diagnostics. Version tags publish downloadable release ZIPs after these checks pass. This does not establish game compatibility, texture visualization or profiling accuracy under Proton; those need a real game run.

## References

The UI workflow and marker semantics were informed by [RenderDoc's event browser](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/event_browser.rst), [pipeline state viewer](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/pipeline_state.rst), and [annotation documentation](https://github.com/baldurk/renderdoc/blob/v1.x/docs/how/how_annotate_capture.rst). RenderDoc was inspected as a source reference; its capture/replay engine is not linked into this add-on. [Microsoft PixEvents](https://github.com/microsoft/PixEvents) provides the public PIX event runtime, and [Intel GPA Framework](https://intel.github.io/gpasdk-doc/) documents Intel's framework. Neither inspector is a dependency of Mod Kit. Existing ReShade and third-party licenses remain in place.
