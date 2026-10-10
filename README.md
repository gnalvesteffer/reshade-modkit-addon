# ReShade Mod Kit Add-on

A standalone ReShade add-on for inspecting and modding game graphics in real time. Built against ReShade's public SDK; no custom injector fork is required.

- Frame browser and pipeline/resource inspection.
- Shader suppression, replacement, HLSL/GLSL editing and draw rules.
- Sampled texture and read-only buffer replacements with file watching.
- **Optional D3D12 GPU timing**, expensive-draw sorting and shader cost summaries.
- Automated shader suppression comparisons, with warmup and frame-time statistics.

Live capture, GPU timing, and CPU upload snapshots are **off by default**. The idle path avoids full draw snapshots and skips replacement scans when no files are installed. BeamNG game performance and the new profiler have not yet been validated at runtime.

## Install

Install ReShade with **full add-on support**, compatible with SDK API 20 / ImGui 1.92.5. Copy `ModKit.addon64` beside the game's ReShade DLL. Open **Home → Mod Kit**.

Use a single Mod Kit instance. If migrating from the old fork, replace its injector with a compatible ReShade build before loading this add-on. Existing `reshade-modkit/<game>/` files and rules are preserved.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

Linux with MinGW:

```sh
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw --parallel
```

See [MODKIT.md](MODKIT.md) for workflows, timing semantics, coverage and limits. Vendored SDK/dependency versions and original licenses are under [vendor](vendor).
