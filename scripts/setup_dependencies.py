import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parent.parent
IMPORT_LIBRARIES = (
    ("Qt5Core.def", "libQt5Core.a"),
    ("Qt5Gui.def", "libQt5Gui.a"),
    ("Qt5Widgets.def", "libQt5Widgets.a"),
    ("Qt5Xml.def", "libQt5Xml.a"),
    ("lmms.def", "liblmms.a"),
    ("libsamplerate-0.def", "libsamplerate.a"),
)


def verify_archive(archive, expected):
    digest = hashlib.sha256()
    with archive.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    actual = digest.hexdigest()
    if actual != expected:
        raise ValueError(f"SHA-256 mismatch for {archive}: expected {expected}, got {actual}")


def cached_archive(dependency, cache, offline=False):
    archive = cache / dependency["archive"]
    if archive.exists():
        verify_archive(archive, dependency["sha256"])
        return archive
    if offline:
        raise RuntimeError(f"Archive missing in offline mode: {archive}")
    cache.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {dependency['name']}: {dependency['url']}")
    output = tempfile.NamedTemporaryFile(dir=cache, prefix=archive.name + ".", suffix=".part", delete=False)
    temporary = Path(output.name)
    try:
        with output, urlopen(dependency["url"], timeout=60) as response:
            shutil.copyfileobj(response, output)
        verify_archive(temporary, dependency["sha256"])
        if archive.exists():
            verify_archive(archive, dependency["sha256"])
        else:
            temporary.rename(archive)
    finally:
        output.close()
        temporary.unlink(missing_ok=True)
    return archive


def validate_member_path(name, archive_root):
    path = PurePosixPath(name.replace("\\", "/"))
    if (not path.parts or path.is_absolute() or path.parts[0] != archive_root
            or any(part == ".." or ":" in part for part in path.parts)):
        raise ValueError(f"Unsafe or unexpected archive path: {name}")


def archive_tool():
    native = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32" / "tar.exe"
    if native.is_file():
        return str(native)
    tool = shutil.which("bsdtar")
    if not tool:
        raise RuntimeError("Windows tar.exe (libarchive) or bsdtar is required to extract the pinned .7z archives")
    return tool


def validate_dependency(directory, dependency):
    missing = [name for name in dependency["required_files"] if not (directory / name).is_file()]
    if missing:
        raise RuntimeError(f"Dependency tree is missing or incomplete: {directory}; missing {', '.join(missing)}. Existing directories are not replaced.")
    if "version_command" in dependency:
        command = dependency["version_command"]
        actual = subprocess.run(
            [str(directory / command[0]), *command[1:]],
            check=True, capture_output=True, text=True,
        ).stdout.strip()
        if actual != dependency["expected_version"]:
            raise RuntimeError(f"Unexpected {dependency['name']} version: {actual}; expected {dependency['expected_version']}")


def ensure_dependency(root, dependency, cache, offline=False):
    destination = root / dependency["destination"]
    if destination.exists():
        validate_dependency(destination, dependency)
        print(f"Keeping existing dependency: {destination}")
        return
    archive = cached_archive(dependency, cache, offline)
    tool = archive_tool()
    members = subprocess.run(
        [tool, "-tf", str(archive)], check=True, capture_output=True,
        text=True, encoding="utf-8", errors="replace",
    ).stdout.splitlines()
    if not members:
        raise ValueError(f"Empty dependency archive: {archive}")
    for member in members:
        validate_member_path(member, dependency["archive_root"])
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".setup-", dir=destination.parent) as directory:
        staging = Path(directory)
        subprocess.run([tool, "-xf", str(archive), "-C", str(staging)], check=True)
        extracted = staging / dependency["archive_root"]
        validate_dependency(extracted, dependency)
        extracted.rename(destination)
    print(f"Installed pinned dependency: {destination}")


def generate_import_libraries(root, regenerate=False):
    directory = root / "sdk" / "lib"
    pending = [(definition, library) for definition, library in IMPORT_LIBRARIES
               if regenerate or not (directory / library).is_file()]
    if not pending:
        print("Keeping existing SDK import libraries")
        return
    tool = root / "tools" / "mingw64" / "bin" / "dlltool.exe"
    for source in [tool, *(root / "sdk" / "def" / definition for definition, _ in pending)]:
        if not source.is_file():
            raise RuntimeError(f"Import-library input is missing: {source}")
    environment = dict(os.environ)
    environment["PATH"] = str(tool.parent) + os.pathsep + environment.get("PATH", "")
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".imports-", dir=directory) as temporary:
        staging = Path(temporary)
        for definition, library in pending:
            output = staging / library
            subprocess.run(
                [str(tool), "-m", "i386:x86-64", "-d", str(root / "sdk" / "def" / definition), "-l", library],
                check=True, env=environment, cwd=staging,
            )
            if not output.is_file() or output.stat().st_size == 0:
                raise RuntimeError(f"dlltool did not generate {library}")
        for _, library in pending:
            destination = directory / library
            if not regenerate and destination.exists():
                raise RuntimeError(f"Refusing to replace an existing import library: {destination}")
            (staging / library).replace(destination)
            print(f"Generated SDK import library: {destination}")


def main():
    parser = argparse.ArgumentParser(description="Set up pinned LMMS 1.2.2 native build dependencies without modifying the installed LMMS")
    parser.add_argument("--cache-dir", type=Path, help="Reuse a shared archive cache instead of tools/downloads")
    parser.add_argument("--offline", action="store_true", help="Require already-cached archives; never download")
    parser.add_argument("--check", action="store_true", help="Check existing dependencies and import libraries without writing files")
    parser.add_argument("--regenerate-sdk", action="store_true", help="Explicitly regenerate existing sdk/lib import libraries from tracked .def files")
    options = parser.parse_args()
    if options.check and options.regenerate_sdk:
        parser.error("--check cannot be combined with --regenerate-sdk")
    dependencies = json.loads((ROOT / "scripts" / "dependencies.json").read_text(encoding="utf-8"))["dependencies"]
    cache = (options.cache_dir or ROOT / "tools" / "downloads").resolve()
    for dependency in dependencies:
        if options.check:
            validate_dependency(ROOT / dependency["destination"], dependency)
        else:
            ensure_dependency(ROOT, dependency, cache, options.offline)
    if options.check:
        for _, library in IMPORT_LIBRARIES:
            path = ROOT / "sdk" / "lib" / library
            if not path.is_file() or path.stat().st_size == 0:
                raise RuntimeError(f"SDK import library is missing or empty: {path}")
    else:
        generate_import_libraries(ROOT, options.regenerate_sdk)
    print("Pinned native dependencies and SDK import libraries are ready")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
