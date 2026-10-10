# ReShade Mod Kit Add-on

A standalone ReShade add-on for inspecting and modding game graphics in real time. Built against ReShade's public SDK; no custom injector fork is required.

- Frame browser and pipeline/resource inspection.
- Shader suppression, replacement, HLSL/GLSL editing and draw rules.
- Persistent per-game shader labels and DXBC/DXIL disassembly.
- Sampled texture and read-only buffer replacements with file watching.
- Texture image previews and on-demand asynchronous D3D12 snapshots.
- **Optional D3D12 GPU timing**, expensive-draw sorting and shader cost summaries.
- Automated shader suppression comparisons, with warmup and frame-time statistics.
- Experimental D3D12 control to run ReShade effects before a selected UI draw.

Live capture, GPU timing, and CPU upload snapshots are **off by default**. The idle path avoids full draw snapshots and skips replacement scans when no files are installed. BeamNG game performance and the new profiler have not yet been validated at runtime.

## Install

Download an installable ZIP from [Releases](https://github.com/gnalvesteffer/reshade-modkit-addon/releases). Preview releases include experimental features. Each ZIP includes the add-on, DXIL disassembler, documentation, licenses and checksums.

Install [official ReShade](https://reshade.me/) with **full add-on support**, compatible with SDK API 20 / ImGui 1.92.5 (ReShade 6.8.0 is the current deployment target). Copy `ModKit.addon64` and the bundled `ModKit-tools` folder beside the game's ReShade DLL. Open **Home → Mod Kit**.

Select a shader, enter a **Label**, and click **Save labels**. Labels appear in searches, shader details, profiling summaries and frame exports. They load from `reshade-modkit/<game>/shader-labels.tsv` on the next run; changed shader bytecode after a game update may need a new label.

Use a single Mod Kit instance. If migrating from the old fork, replace its injector with a compatible ReShade build before loading this add-on. Existing `reshade-modkit/<game>/` files and rules are preserved.

Select a texture to show its base mip preview. D3D12 GPU-only textures request a snapshot at the next supported graphics-queue transition; use **Snapshot GPU contents** to refresh. Static textures without later transitions can remain pending. **Reset all suppression** unticks every shader suppression control and cancels an active comparison; saved `rules.ini` skip rules remain separate mods.

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

See [MODKIT.md](MODKIT.md) for workflows, timing semantics, coverage and limits. Vendored SDK/dependency versions and original licenses are under [vendor](vendor).

## Repository skills

Task-specific guidance for contributors and coding agents lives in [`.agents/skills/`](.agents/skills/README.md):

- [Build and release](.agents/skills/addon-build-release/SKILL.md)
- [Graphics capture debugging](.agents/skills/graphics-capture-debugging/SKILL.md)
- [Shader and texture modding](.agents/skills/shader-texture-modding/SKILL.md)
- [Deploying to a game](.agents/skills/game-deployment/SKILL.md)

## CI and releases

GitHub Actions runs on pushes to `main` and pull requests that change source, vendored dependencies, build configuration, packaging scripts or checks. Documentation-only and skill-only changes do not start a build. Version tags still run release checks, and `workflow_dispatch` can be used for a manual run. CI builds the Windows x64 add-on, checks profile/label/execution-point persistence and texture preview guards, and validates the install layout and SHA256 manifests. Build artifacts include the raw add-on and an installable ZIP. These checks do not run a game or measure GPU performance.

To publish a version, push an annotated `vX.Y.Z` tag. The tag workflow repeats the checks and publishes the ZIP and its SHA256 file as a GitHub release after both jobs pass. Tags containing a hyphen, such as `v0.3.0-alpha.1`, publish a prerelease. Release jobs alone receive repository write permission.

Run Windows persistence checks locally with:

```powershell
cmake -S . -B build -A x64 -DMODKIT_BUILD_CHECKS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python -m unittest discover -s tests -p 'test_*.py' -v
```
