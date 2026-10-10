---
name: game-deployment
description: Install, update, verify, or revert the ReShade Mod Kit add-on for a user-selected game on Windows or Linux/Proton. Keep deployment game-agnostic and identify the actual executable and graphics API first.
---

# Deploy to a game

Use this skill when installing a built Mod Kit package into a game, updating an existing installation, or diagnosing a deployment that fails to load. The user must identify the game or provide its install location; do not assume a particular title or default path.

## Identify the installation

1. Find the game's actual rendering executable, not just its launcher. Check the launcher configuration, game settings, process name, or user-provided location. Games with multiple graphics backends can use different executables or launch arguments.
2. Determine operating system, architecture, graphics API, and whether the game runs natively or through a compatibility layer such as Proton.
3. Locate the ReShade proxy/runtime DLL that is loaded by that executable. Use the official ReShade installer configured for the game's actual executable and API when ReShade has not already been installed.
4. Confirm that the ReShade build has full add-on support and matches the add-on SDK/API requirements documented in the root README.

## Install or update

1. Build or obtain the install ZIP. Confirm it contains `ModKit.addon64` and `ModKit-tools/`.
2. Before replacing files, preserve the existing ReShade files and configuration. Do not overwrite unrelated game files or remove other add-ons.
3. Copy the add-on and its tool folder beside the ReShade DLL loaded by the game. Keep the tool folder name and layout intact.
4. If there is an older Mod Kit fork or duplicate inspector, disable/remove only that known Mod Kit component so two copies do not hook the same process. Preserve per-game `reshade-modkit/` data unless the user asks to reset it.
5. For Proton, deploy the Windows ReShade DLL/add-on in the game's Windows installation tree used by Proton. Do not copy a native Linux library into that tree. Account for launcher-managed prefixes and API translation when checking the active DLL.

Deployment changes are reversible. Keep backups or a clear record of replaced files so rollback can restore the previous runtime and add-ons without disturbing user settings.

## Verify and revert

Launch the exact game executable with the selected graphics API. Check the ReShade overlay, **Home → Mod Kit**, and status text. Confirm the reported API and try a one-frame capture before enabling live capture or profiling. If it does not load, check DLL placement, architecture, ReShade add-on support, duplicate injectors, graphics API selection, and logs before changing files again.

To revert, close the game, restore the recorded ReShade files, and remove only the Mod Kit add-on and its tools. Leave user-created shader labels, replacements, captures and rules untouched unless the user explicitly requests their removal.

Report the executable, API, files changed, verification result and rollback location. Do not claim deployment success solely because files were copied.
