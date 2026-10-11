import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("modkit_package", Path(__file__).resolve().parents[1] / "scripts/package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageChecks(unittest.TestCase):
    def test_install_layout_and_checksums(self):
        with tempfile.TemporaryDirectory(prefix="modkit-package-check-") as temporary:
            root = Path(temporary)
            binary = root / "binary"
            (binary / "ModKit-tools").mkdir(parents=True)
            (binary / "ModKit.addon64").write_bytes(b"fixture-addon")
            for name in ("dxcompiler.dll", "dxil.dll", "LICENSE-MS.txt", "LICENSE-LLVM.txt"):
                (binary / "ModKit-tools" / name).write_bytes(b"fixture")
            archive = package.build_package(binary, root / "dist", "v0.3.0-alpha.1")
            checksum = archive.with_suffix(".zip.sha256").read_text().split()[0]
            self.assertEqual(checksum, hashlib.sha256(archive.read_bytes()).hexdigest())
            with zipfile.ZipFile(archive) as contents:
                self.assertIn("ModKit.addon64", contents.namelist())
                self.assertIn("ModKit-tools/dxcompiler.dll", contents.namelist())
                self.assertIn("ModKit-tools/dxil.dll", contents.namelist())
                self.assertIn("licenses/dxc/LICENSE-MS.txt", contents.namelist())
                self.assertEqual(json.loads(contents.read("build-info.json"))["reshade_sdk_api"], 20)
                for line in contents.read("SHA256SUMS").decode().splitlines():
                    expected, path = line.split("  ", 1)
                    self.assertEqual(expected, hashlib.sha256(contents.read(path)).hexdigest(), path)

    def test_reject_invalid_version(self):
        for version in ("../../escape", "main", "v1.2", "v1.2.3;echo"):
            with self.assertRaises(ValueError):
                package.build_package(Path("missing"), Path("unused"), version)

    def test_require_disassembler(self):
        with tempfile.TemporaryDirectory(prefix="modkit-package-check-") as temporary:
            with self.assertRaises(FileNotFoundError):
                package.build_package(Path(temporary), Path(temporary) / "dist", "v0.3.0")

    def test_require_dxil_validator(self):
        with tempfile.TemporaryDirectory(prefix="modkit-package-check-") as temporary:
            binary = Path(temporary)
            (binary / "ModKit.addon64").write_bytes(b"fixture")
            (binary / "ModKit-tools").mkdir()
            (binary / "ModKit-tools/dxcompiler.dll").write_bytes(b"fixture")
            with self.assertRaisesRegex(FileNotFoundError, "dxil.dll"):
                package.build_package(binary, binary / "dist", "v0.3.0")


if __name__ == "__main__":
    unittest.main()
