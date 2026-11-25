#!/usr/bin/env python3
"""Assemble a relocatable Intel macOS app and its corresponding source."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")
DYLIB_COMMANDS = {
    "LC_LAZY_LOAD_DYLIB", "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB",
    "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB",
}
DEPENDENCIES = {
    "Scintilla": (
        "scintilla569.tgz",
        "1622546ce1d3804b589292f1adae98ad1d87fa8c43bb401edc098adae12dcacb",
        ("scintilla/License.txt",),
    ),
    "Lexilla": (
        "lexilla555.tgz",
        "fa4093ad5c48acf49a6f3eca1b75143a5c824ef1a6f5b0ee59e8c3d6a970f229",
        ("lexilla/License.txt",),
    ),
    "QTermWidget": (
        "qtermwidget-2.4.0.tar.xz",
        "ceb7d1991ecc0e75b751b6b7786c4962598d367c6e9a0b55f78fc12a49f25304",
        tuple(f"qtermwidget-2.4.0/{name}" for name in
              ("AUTHORS", "LICENSE", "LICENSE.LGPL2+", "LICENSE.BSD-3-clause", "COPYING-CMAKE-SCRIPTS")),
    ),
}


def run(*args, capture=False):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.STDOUT if capture else None)
    if result.returncode and capture:
        raise RuntimeError(f"{args[0]} exited with status {result.returncode}:\n{result.stdout}")
    result.check_returncode()
    return result.stdout


def load_commands(binary):
    command = None
    for line in run("/usr/bin/otool", "-l", binary, capture=True).splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            command = line.split()[1]
        elif line.startswith(("name ", "path ")) and " (offset " in line:
            yield command, line.split(" ", 1)[1].rsplit(" (offset ", 1)[0]


def bundle_binaries(app):
    contents = app / "Contents"
    executable = contents / "MacOS/NebulaCode"
    yield executable
    for directory in (contents / "Frameworks", contents / "PlugIns"):
        for candidate in sorted(directory.rglob("*")):
            if candidate.is_symlink() or not candidate.is_file():
                continue
            if candidate.suffix == ".dylib" or candidate.parent.parent.name == "Versions":
                yield candidate


def bundled_reference(app, binary, name):
    contents = app / "Contents"
    if name.startswith("@rpath/"):
        target = contents / "Frameworks" / name.removeprefix("@rpath/")
    elif name.startswith("@executable_path/"):
        target = contents / "MacOS" / name.removeprefix("@executable_path/")
    elif name.startswith("@loader_path/"):
        target = binary.parent / name.removeprefix("@loader_path/")
    else:
        raise RuntimeError(f"External library reference in {binary.name}: {name}")
    target = target.resolve()
    if not target.is_relative_to(contents) or not target.is_file():
        raise RuntimeError(f"Missing bundled library for {binary.name}: {name}")
    return "@executable_path/" + os.path.relpath(target, contents / "MacOS")


def relocate_and_verify(app):
    binaries = list(bundle_binaries(app))
    for binary in binaries:
        architectures = run("/usr/bin/lipo", "-archs", binary, capture=True).strip()
        if architectures != "x86_64":
            raise RuntimeError(f"Expected Intel x86_64 in {binary.name}, found {architectures}")
        edits = []
        for command, name in load_commands(binary):
            if command == "LC_RPATH":
                edits.extend(("-delete_rpath", name))
            elif command in DYLIB_COMMANDS and not name.startswith(SYSTEM_PREFIXES):
                relative = bundled_reference(app, binary, name)
                if relative != name:
                    edits.extend(("-change", name, relative))
            elif command == "LC_ID_DYLIB":
                name_in_bundle = binary.relative_to(app / "Contents")
                edits.extend(("-id", "@executable_path/../" + name_in_bundle.as_posix()))
        if edits:
            run("/usr/bin/install_name_tool", *edits, binary, capture=True)

    for binary in binaries:
        for command, name in load_commands(binary):
            if command == "LC_RPATH":
                raise RuntimeError(f"Unexpected run path in {binary.name}: {name}")
            if command in DYLIB_COMMANDS and not name.startswith(SYSTEM_PREFIXES):
                if not name.startswith("@executable_path/"):
                    raise RuntimeError(f"Unexpected library reference in {binary.name}: {name}")
                bundled_reference(app, binary, name)

    for binary in binaries[1:]:
        if not any(parent.suffix == ".framework" for parent in binary.parents):
            run("/usr/bin/codesign", "--force", "--sign", "-", binary, capture=True)
    for framework in sorted((app / "Contents/Frameworks").glob("*.framework")):
        run("/usr/bin/codesign", "--force", "--sign", "-", framework, capture=True)
    run("/usr/bin/codesign", "--force", "--sign", "-", app, capture=True)
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)
    print(f"Verified {len(binaries)} Intel Mach-O binaries and bundle-local linkage.", flush=True)


def brew_formulae(deployment_log):
    formulae = set()
    for copied in re.findall(r'copied: "([^"]+)"', deployment_log):
        path = Path(copied).resolve()
        if "Cellar" in path.parts:
            formulae.add(path.parts[path.parts.index("Cellar") + 1])
    return sorted(formulae)


def stage_dependency_sources(archive_dir, source, notices):
    sources = source / "upstream"
    sources.mkdir()
    for name, (archive, digest, license_paths) in DEPENDENCIES.items():
        input_path = archive_dir / archive
        with input_path.open("rb") as content:
            if hashlib.file_digest(content, "sha256").hexdigest() != digest:
                raise RuntimeError(f"Source archive checksum mismatch: {input_path}")
        shutil.copy2(input_path, sources)
        component_notices = notices / name
        component_notices.mkdir(parents=True)
        with tarfile.open(input_path) as bundle:
            for license_path in license_paths:
                with bundle.extractfile(license_path) as original:
                    (component_notices / Path(license_path).name).write_bytes(original.read())


def stage_project_source(source):
    project = source / "NebulaCode"
    project.mkdir()
    for name in ("CMakeLists.txt", "CMakePresets.json", "LICENSE", "README.md", "plan.md", ".gitignore"):
        shutil.copy2(ROOT / name, project)
    for name in ("cmake", "resources", "scripts", "src", "tests", "docs"):
        directory = ROOT / name
        if directory.is_dir():
            shutil.copytree(directory, project / name,
                            ignore=shutil.ignore_patterns(".DS_Store", "__pycache__"))


def smoke_test(app):
    with tempfile.TemporaryDirectory(prefix="nebula-clean-") as folder:
        clean = Path(folder).resolve()
        relocated = clean / app.name
        run("/usr/bin/ditto", app, relocated)
        workspace = clean / "workspace"
        workspace.mkdir()
        (workspace / "check.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        environment = {
            "HOME": str(clean),
            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
            "TMPDIR": str(clean),
            "XDG_CONFIG_HOME": str(clean / "config"),
            "XDG_DATA_HOME": str(clean / "data"),
            "QT_QPA_PLATFORM": "offscreen",
            "QT_DEBUG_PLUGINS": "1",
        }
        with (clean / "launch.log").open("w+") as log:
            process = subprocess.Popen([relocated / "Contents/MacOS/NebulaCode", workspace / "check.cpp"],
                                       cwd=workspace, env=environment, stdout=log, stderr=log)
            try:
                time.sleep(2)
                if process.poll() is not None:
                    log.seek(0)
                    raise RuntimeError(f"Packaged app did not remain running:\n{log.read()}")
                log.seek(0)
                launch_log = log.read()
                plugin = relocated / "Contents/PlugIns/platforms/libqoffscreen.dylib"
                if f'"{plugin}" loaded library' not in launch_log:
                    raise RuntimeError("The relocated app did not load its offscreen platform plugin.")
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        print("Launched relocated app offscreen with a system-only PATH.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/release")
    parser.add_argument("--archive-dir", type=Path, default=ROOT / "build/vendor")
    parser.add_argument("--output", type=Path, default=ROOT / "build/dist")
    parser.add_argument("--macdeployqt", default=shutil.which("macdeployqt"))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error(f"output already exists: {output}")
    macdeployqt = shutil.which(args.macdeployqt) if args.macdeployqt else None
    if not macdeployqt:
        parser.error("macdeployqt was not found. Set --macdeployqt.")
    original = build / "NebulaCode.app"
    with (original / "Contents/Info.plist").open("rb") as plist:
        version = plistlib.load(plist)["CFBundleShortVersionString"]
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nebula-package-", dir=output.parent) as staging:
        package = Path(staging) / "package"
        package.mkdir()
        app = package / original.name
        run("/usr/bin/ditto", original, app)
        deployment_log = run(macdeployqt, app, f"-libpath={build}",
                             "-no-plugins", "-no-strip", "-no-codesign", "-verbose=2",
                             capture=True)
        if any(line.lstrip().startswith("ERROR:") for line in deployment_log.splitlines()):
            raise RuntimeError(deployment_log)
        qtpaths = Path(macdeployqt).resolve().with_name("qtpaths")
        qt_plugins = Path(run(qtpaths, "--query", "QT_INSTALL_PLUGINS", capture=True).strip())
        platforms = app / "Contents/PlugIns/platforms"
        platforms.mkdir(parents=True)
        for plugin in ("libqcocoa.dylib", "libqoffscreen.dylib"):
            shutil.copy2(qt_plugins / "platforms" / plugin, platforms)
        formulae = brew_formulae(deployment_log)
        print("Deployed runtime libraries from " + ", ".join(formulae), flush=True)
        resources = app / "Contents/Resources"
        resources.mkdir(exist_ok=True)
        # Qt may resolve its prefix from the bundle or from the executable's directory.
        (resources / "qt.conf").write_text("[Paths]\nPlugins = PlugIns, ../PlugIns\n", encoding="utf-8")

        source = package / f"NebulaCode-{version}-source"
        source.mkdir()
        notices = source / "notices"
        notices.mkdir()
        stage_project_source(source)
        stage_dependency_sources(args.archive_dir.resolve(), source, notices)
        if formulae:
            run(sys.executable, ROOT / "scripts/brew-sources.py",
                "--archive-dir", args.archive_dir.resolve(), source, *formulae)
        shutil.copytree(notices, resources / "Notices")
        shutil.copy2(ROOT / "LICENSE", resources / "Notices/NebulaCode.txt")
        relocate_and_verify(app)
        smoke_test(app)

        app_zip = package / f"NebulaCode-{version}-macos-x86_64.zip"
        run("/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app, app_zip)
        source_archive = package / f"NebulaCode-{version}-source.tar.gz"
        with tarfile.open(source_archive, "w:gz", compresslevel=3) as archive:
            archive.add(source, arcname=source.name)
        shutil.rmtree(source)
        package.rename(output)
    print(f"Release artifacts: {output}")


if __name__ == "__main__":
    main()
