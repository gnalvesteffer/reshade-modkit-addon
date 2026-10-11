---
name: shader-texture-modding
description: Create, inspect, edit, replace, label, suppress, or validate shaders and textures using the ReShade Mod Kit add-on and its on-disk mod format.
---

# Work with shader and texture mods

Use this skill for user-facing mod workflows, replacement format changes, editor behavior, labels, suppression rules, texture previews, or shader/resource replacement bugs.

## Identify the resource

1. Capture a frame or enable live capture in the Mod Kit, then select the relevant action or resource.
2. Compare shader IDs, bound stages and resource IDs across likely actions. If useful, suppress a shader briefly to identify its visible contribution, then restore suppression.
3. Use a persistent shader label when the user has identified a shader's role. Labels are tied to original bytecode hashes and do not enable replacement or suppression by themselves.
4. Treat `session-<serial>` resources as temporary identifiers; they may change after a game restart when no content hash could be captured.

## Edit or create replacements

- Follow the per-game directory layout and exact extensions documented in `MODKIT.md`.
- Preserve the original shader stage, entry point and input/output/resource interface. Original HLSL source is not reconstructed from compiled bytecode.
- The built-in editor supports GLSL editing, DXBC Shader Model 4/5 HLSL compilation, and DXIL Shader Model 6 compilation with bundled DXC/validator tools. SPIR-V compilation needs external tools. Use the original interface and disassembly as inspection data, not as recoverable original source. Generated pixel starters output a diagnostic color and do not preserve original lighting.
- Use **Open replacement editor** or **Import source...** for shaders, then **Compile and apply** for HLSL. For compatible RGBA8/BGRA8 texture snapshots, **Edit PNG externally** opens an editable replacement; **Import PNG...** installs matching images. Native texture replacements must contain the complete mip chain.
- When importing game-provided HLSL, configure include directories and the original variant's defines in the editor's compiler settings. The original source directory is added automatically; root-relative includes may need the game/source root as well. These settings persist in the shader's `.compile.ini`.
- Texture PNG replacement is limited to matching dimensions and RGBA8/BGRA8 formats. Native `.bin` replacements must match the supported format layout and include every mip level. Buffer replacements must match the original byte size.
- Replacement and suppression affect supported intercepted work; they do not rewrite every game resource use or every API path. Consult the coverage table in `MODKIT.md` before promising compatibility.

## Validate safely

1. Keep a copy of the original mod file before editing when practical.
2. Check the Mod Kit status and whether the replacement compiled/loaded. Failed compilation should retain the previous working pipeline; confirm the actual behavior in the relevant code path before changing that guarantee.
3. Compare the same scene and action with the mod enabled and disabled. Use **Reset all suppression** to clear session suppression controls; persistent skip rules in `rules.ini` must be handled separately.
4. For texture previews, distinguish a displayed snapshot from the resource's exact contents at the selected draw. D3D12 GPU-only snapshots can reflect the next qualifying transition.

Explain format, API and capture limitations in mod author guidance. Never treat a visible shader or texture in the inspector as proof that its resource can be replaced on every backend.
