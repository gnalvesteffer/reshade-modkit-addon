"""Install the pinned Microsoft DXC compiler, validator and disassembler."""
import argparse
import hashlib
from pathlib import Path
import urllib.request
import zipfile

URL = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip"
SHA256 = "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1"

parser = argparse.ArgumentParser()
parser.add_argument("--destination", type=Path, required=True, help="Package directory containing ModKit.addon64")
parser.add_argument("--archive", type=Path, help="Use an already downloaded DXC archive")
parser.add_argument("--architecture", choices=("x64", "x86"), default="x64")
args = parser.parse_args()
args.destination.mkdir(parents=True, exist_ok=True)
archive = args.archive or args.destination / "dxc-download.zip"
if not archive.is_file():
    urllib.request.urlretrieve(URL, archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
    raise RuntimeError("DXC archive SHA256 does not match the pinned release")
tools = args.destination / "ModKit-tools"
tools.mkdir(exist_ok=True)
with zipfile.ZipFile(archive) as package:
    names = {name.replace("\\", "/"): name for name in package.namelist()}
    for dll in ("dxcompiler.dll", "dxil.dll"):
        source = "bin/" + args.architecture + "/" + dll
        (tools / dll).write_bytes(package.read(names[source]))
    for name in ("LICENSE-MS.txt", "LICENSE-LLVM.txt"):
        (tools / name).write_bytes(package.read(names[name]))
if not args.archive:
    archive.unlink()
print(tools)
