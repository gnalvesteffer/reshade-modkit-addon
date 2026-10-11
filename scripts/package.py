"""Create an installable add-on ZIP with licenses, revision and checksums."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile


def build_package(binary_dir, output_dir, version):
    if not re.fullmatch(r"(?:v\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?|[0-9a-f]{40})", version):
        raise ValueError("Version must be a vX.Y.Z tag, optional prerelease, or full commit SHA")
    repo = Path(__file__).resolve().parents[1]
    binary_dir, output_dir = Path(binary_dir), Path(output_dir)
    for required in ("ModKit.addon64", "ModKit-tools/dxcompiler.dll", "ModKit-tools/dxil.dll", "ModKit-tools/LICENSE-MS.txt", "ModKit-tools/LICENSE-LLVM.txt"):
        if not (binary_dir / required).is_file():
            raise FileNotFoundError(f"Required package file missing: {required}")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    output_dir.mkdir(parents=True, exist_ok=True)
    archive = output_dir / f"ModKit-addon-x64-{version}.zip"
    with tempfile.TemporaryDirectory(prefix="modkit-package-") as temporary:
        package = Path(temporary)
        shutil.copy2(binary_dir / "ModKit.addon64", package / "ModKit.addon64")
        shutil.copytree(binary_dir / "ModKit-tools", package / "ModKit-tools")
        for name in ("README.md", "MODKIT.md", "LICENSE.md"):
            shutil.copy2(repo / name, package / name)
        shutil.copy2(repo / "vendor/versions.json", package / "versions.json")
        for dependency, name in (("reshade", "LICENSE.md"), ("imgui", "LICENSE.txt"), ("stb", "LICENSE"), ("minhook", "LICENSE.txt"), ("dxc", "LICENSE-MS.txt"), ("dxc", "LICENSE-LLVM.txt")):
            destination = package / "licenses" / dependency / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repo / "vendor" / dependency / name, destination)
        (package / "REVISION.txt").write_text(revision + "\n", encoding="utf-8")
        info = {"version": version, "revision": revision, "architecture": "x64", "reshade_sdk_api": 20, "imgui_version": "1.92.5", "experimental": True}
        (package / "build-info.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
        checksums = "".join(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.relative_to(package).as_posix() + "\n" for path in sorted(package.rglob("*")) if path.is_file())
        (package / "SHA256SUMS").write_text(checksums, encoding="utf-8")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            for path in sorted(package.rglob("*")):
                if path.is_file():
                    output.write(path, path.relative_to(package).as_posix())
    archive.with_suffix(".zip.sha256").write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n", encoding="utf-8")
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    parser.add_argument("--version", required=True)
    arguments = parser.parse_args()
    print(build_package(arguments.binary_dir, arguments.output_dir, arguments.version))
