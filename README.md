# ReShade Mod Kit Add-on

A standalone ReShade add-on for inspecting and modding game graphics in real time. Built against ReShade's public SDK; no custom injector fork is required.

- Frame browser and pipeline/resource inspection.
- Shader suppression, replacement, HLSL/GLSL editing and draw rules.
- Guided replacement editor with original shader interfaces, pixel shader starters, compiler diagnostics and DXBC/DXIL compilation.
- Persistent per-game shader labels and DXBC/DXIL disassembly.
- Sampled texture and read-only buffer replacements with file watching.
- Texture image previews and on-demand asynchronous D3D12 snapshots.
- **Optional D3D12 GPU timing**, expensive-draw sorting and shader cost summaries.
- Automated shader suppression comparisons, with warmup and frame-time statistics.
- Experimental D3D12 control to run ReShade effects before a selected UI draw.

Live capture, GPU timing, and CPU upload snapshots are **off by default**. The idle path avoids full draw snapshots and skips replacement scans when no files are installed. BeamNG game performance and the new profiler have not yet been validated at runtime.

## Install

Download an installable ZIP from [Releases](https://github.com/gnalvesteffer/reshade-modkit-addon/releases). Preview releases include experimental features. Each ZIP includes the add-on, DXIL compiler/validator/disassembler, documentation, licenses and checksums.

Install [official ReShade](https://reshade.me/) with **full add-on support**, compatible with SDK API 20 / ImGui 1.92.5 (ReShade 6.8.0 is the current deployment target). Copy `ModKit.addon64` and the bundled `ModKit-tools` folder beside the game's ReShade DLL. Open **Home → Mod Kit**.

Select a shader, enter a **Label**, and click **Save labels**. Labels appear in searches, shader details, profiling summaries and frame exports. They load from `reshade-modkit/<game>/shader-labels.tsv` on the next run; changed shader bytecode after a game update may need a new label.

Use a single Mod Kit instance. If migrating from the old fork, replace its injector with a compatible ReShade build before loading this add-on. Existing `reshade-modkit/<game>/` files and rules are preserved.

Select a texture to show its base mip preview. D3D12 GPU-only textures request a snapshot at the next supported graphics-queue transition; use **Snapshot GPU contents** to refresh. Static textures without later transitions can remain pending. **Reset all suppression** unticks every shader suppression control and cancels an active comparison; saved `rules.ini` skip rules remain separate mods.

To edit a Direct3D shader, choose **Open replacement editor**. Compiled game shaders usually do not contain original HLSL: the editor shows their reflected inputs, outputs, resource registers and disassembly for reference. Supported pixel shaders start with a generated magenta-output diagnostic replacement. Edit it and choose **Compile and apply**, or **Import source...** / **Import compiled replacement...** to use your own compatible shader. Shader Model 4/5 uses D3DCompiler; Shader Model 6 uses the bundled DXC compiler and validator. Compilation errors appear in the editor and preserve the previous compiled replacement.

Sources with headers or variant macros can use **Compiler settings: includes and defines**. Source import adds the original directory as an include path; entry point, target, include paths and definitions persist beside the source in `.compile.ini`.

For supported RGBA8/BGRA8 textures, choose **Edit PNG externally** after capturing pixels, or **Import PNG...** to install an image of the same dimensions. Saving the image reloads it when file watching is enabled. Other texture formats currently require a complete native `.bin` mip chain created with external tools; their panels explain the required workflow.

## Build

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

CMake generates MinGW-compatible ReShade SDK headers in the build directory. These preserve MSVC's virtual aggregate return convention for resource and back-buffer queries; bypassing them can crash capture against the official ReShade runtime. The vendored SDK stays unchanged.

See [MODKIT.md](MODKIT.md) for workflows, timing semantics, coverage and limits. Vendored SDK/dependency versions and original licenses are under [vendor](vendor).

## Repository skills

Task-specific guidance for contributors and coding agents lives in [`.agents/skills/`](.agents/skills/README.md):

- [Build and release](.agents/skills/addon-build-release/SKILL.md)
- [Graphics capture debugging](.agents/skills/graphics-capture-debugging/SKILL.md)
- [Shader and texture modding](.agents/skills/shader-texture-modding/SKILL.md)
- [Deploying to a game](.agents/skills/game-deployment/SKILL.md)

## CI and releases

GitHub Actions runs on pushes to `main` and pull requests that change source, vendored dependencies, build configuration, packaging scripts or checks. Documentation-only and skill-only changes do not start a build. Version tags still run release checks, and `workflow_dispatch` can be used for a manual run. CI builds the Windows x64 add-on, checks profile/label/execution-point persistence, texture preview guards and DXBC/DXIL shader editing, and validates the install layout and SHA256 manifests. Build artifacts include the raw add-on and an installable ZIP. These checks do not run a game or measure GPU performance.

To publish a version, push an annotated `vX.Y.Z` tag. The tag workflow repeats the checks and publishes the ZIP and its SHA256 file as a GitHub release after both jobs pass. Tags containing a hyphen, such as `v0.3.0-alpha.1`, publish a prerelease. Release jobs alone receive repository write permission.

Run Windows persistence checks locally with:

```powershell
cmake -S . -B build -A x64 -DMODKIT_BUILD_CHECKS=ON
cmake --build build --config Release --parallel
python scripts/fetch-dxc.py --destination build/Release
ctest --test-dir build -C Release --output-on-failure
python -m unittest discover -s tests -p 'test_*.py' -v
```
