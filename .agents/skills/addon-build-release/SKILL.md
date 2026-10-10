---
name: addon-build-release
description: Build, package, validate, or prepare a release of the ReShade Mod Kit add-on. Use for local builds, CI failures, install ZIPs, version tags, and release workflow changes.
---

# Build and release the add-on

Use this skill when changing or producing the add-on build or its distributable package.

## Establish the build context

1. Read the root `README.md`, `MODKIT.md`, `.github/workflows/build.yml`, `CMakeLists.txt`, and packaging scripts relevant to the requested change.
2. Check `git status` before building or editing. Keep existing user changes intact.
3. Confirm the expected ReShade SDK/API and platform from the project configuration. The supported add-on build is Windows x64; Linux development can cross-compile with the MinGW toolchain.

## Build and package

For a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
python scripts/fetch-dxc.py --destination build/Release
```

For Linux with MinGW:

```sh
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw --parallel
python3 scripts/fetch-dxc.py --destination build-mingw
```

Use the repository packaging script or CI workflow to create the install ZIP. Keep the add-on, required tools, notices/licenses, build metadata and SHA256 manifest consistent with the existing package layout. DXC is pinned and hash-checked by `scripts/fetch-dxc.py`; do not silently replace its version or checksum.

## Validation

- For a Windows code change, follow the checks in `.github/workflows/build.yml`, including the persistence checks and CTest invocation where available.
- Run the package and checksum checks when package contents or scripts change.
- Review the produced ZIP contents and ensure the add-on is beside the bundled `ModKit-tools` directory as expected by installation instructions.
- Report which checks ran and any platform-specific checks that could not run. CI compilation does not prove in-game compatibility or GPU behavior.

Do not create or push a version tag unless the user requested a release. Release tags use `vX.Y.Z`; tags containing a hyphen publish prereleases. Check the current workflow before relying on these details because CI configuration can change.
