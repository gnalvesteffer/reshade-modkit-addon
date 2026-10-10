---
name: graphics-capture-debugging
description: Diagnose missing, incomplete, or incorrect Mod Kit frame actions, shader/resource bindings, texture snapshots, early-effect execution points, or GPU timings in a game.
---

# Debug graphics capture behavior

Use this skill when the user reports missing actions, unexplained bindings, a failed execution point, inaccurate timings, visual corruption, or a capture-related performance issue.

## Collect a focused reproduction

Record the game executable, graphics API, operating system/runtime (including Proton or translation layers), ReShade version and add-on support status, selected action or resource ID, capture toggles, and the exact expected versus observed result. Determine whether the issue occurs every run or only with a particular scene or action. Do not assume a particular game or API.

Use the Mod Kit status text and exported frame data where available. Ask for a log or capture export only when it helps distinguish the relevant cases. Avoid enabling live capture, resource-binding capture, CPU upload capture, or GPU timing by default: each adds work and can change performance.

## Trace from UI to API event

1. Read the relevant section of `MODKIT.md` first. It documents API-specific support and known constraints.
2. Trace the displayed value through the UI and state model to the ReShade event callback or native API hook that produces it.
3. Check whether capture was active when commands were recorded. On explicit APIs, cached command lists may not be observed until the game records them again; first captures can omit commands recorded before capture began.
4. Separate a capture gap from an unsupported API path, an event filtered by the UI, and a resource whose contents were never captured.
5. When investigating a regression, inspect the relevant implementation and existing checks before changing behavior. Keep hook work inactive when the feature is disabled where the current design permits it.

## Interpret results carefully

- D3D12 GPU timing uses asynchronous timestamp readback and is opt-in. It estimates intervals around supported recorded actions, not isolated hardware cost; cached command lists can retain inserted query commands.
- Suppression comparisons use CPU-observed present intervals and can include CPU cost, frame caps, VSync and scheduling. They are not GPU timings.
- D3D12 texture snapshots depend on a qualifying observed legacy transition; a static GPU-only texture may stay pending.
- Running ReShade effects before a selected draw is experimental D3D12 functionality with strict target and command-list requirements documented in `MODKIT.md`.
- Engine marker names are only available where an implemented native path exposes them; public ReShade events alone do not provide PIX/GPA labels.

State the scope and evidence for any diagnosis. Do not generalize one game/API observation into support for every graphics backend.
