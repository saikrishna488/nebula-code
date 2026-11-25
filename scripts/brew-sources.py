#!/usr/bin/env python3
"""Collect the exact source and build inputs for deployed Homebrew libraries."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parent.parent
SOURCE_URL = re.compile(r'^\s*url "([^"]+)"', re.MULTILINE)
CHECKSUM = re.compile(r'^\s*sha256 "([a-f0-9]{64})"', re.MULTILINE)
RESOURCE = re.compile(r'^( +)resource "([^"]+)"(?:[^\n]*) do\n(.*?)^\1end\b',
                      re.MULTILINE | re.DOTALL)
PATCH = re.compile(r'^( +)patch do\n(.*?)^\1end\b', re.MULTILINE | re.DOTALL)


def source_entry(text):
    url = SOURCE_URL.search(text)
    sha256 = CHECKSUM.search(text)
    if not url or not sha256:
        raise RuntimeError("A formula source entry has no URL or SHA-256.")
    return url[1], sha256[1]


def verified_archive(inputs, url, digest):
    filename = PurePosixPath(urlparse(url).path).name
    candidates = [path for path in inputs if path.is_file() and
                  (path.name.endswith(filename) or
                   (filename.startswith("v") and path.name.endswith("-" + filename[1:])))]
    for path in candidates:
        with path.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() == digest:
                return path
    raise RuntimeError(f"Missing exact cached input for {url} (SHA-256 {digest})")


def copy_download(inputs, destination, url, digest):
    original = verified_archive(inputs, url, digest)
    output = destination / original.name.split("--", 1)[-1]
    shutil.copy2(original, output)
    return output


def copy_notices(archive_path, keg, destination):
    destination.mkdir(parents=True)
    for source in sorted(keg.iterdir()):
        if source.is_file() and re.match(r"^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE)", source.name, re.I):
            shutil.copy2(source, destination)
    with tarfile.open(archive_path) as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            if not member.isfile() or ".." in path.parts or path.is_absolute():
                continue
            explicit_directory = "LICENSES" in path.parts
            if not explicit_directory and (len(path.parts) > 3 or not re.match(
                    r"^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE)", path.name, re.I)):
                continue
            relative = Path(*path.parts[1:]) if len(path.parts) > 1 else Path(path.name)
            output = destination / "upstream" / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as original:
                output.write_bytes(original.read())
    if not any(destination.iterdir()):
        raise RuntimeError(f"No license notices found in {archive_path.name}")


def collect_formula(keg, name, inputs, source):
    recipe = next((keg / ".brew").glob("*.rb"))
    ruby = recipe.read_text(encoding="utf-8")
    component = source / "homebrew" / name
    component.mkdir(parents=True)
    shutil.copy2(recipe, component)
    url, digest = source_entry(ruby)
    archive = copy_download(inputs, component, url, digest)

    install = ruby.split("def install", 1)[-1].split("\n  test do", 1)[0]
    for _, resource_name, definition in RESOURCE.findall(ruby):
        staged = f'resource("{resource_name}").stage'
        if any(staged in line and "if OS.linux?" not in line for line in install.splitlines()):
            copy_download(inputs, component, *source_entry(definition))
    for _, body in PATCH.findall(ruby):
        patch_file = re.search(r'^\s*file "([^"]+)"', body, re.MULTILINE)
        if patch_file:
            receipt = json.loads((keg / "INSTALL_RECEIPT.json").read_text(encoding="utf-8"))
            installed_formula = Path(receipt["source"]["path"])
            tap = next(p for p in installed_formula.parents if (p / "Formula").is_dir())
            target = component / patch_file[1]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(tap / patch_file[1], target)
        else:
            copy_download(inputs, component, *source_entry(body))

    copy_notices(archive, keg, source / "notices" / name)
    print(f"Collected {name} {keg.name} source and notices.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive-dir", type=Path, default=ROOT / "build/vendor")
    parser.add_argument("source", type=Path)
    parser.add_argument("formulae", nargs="+")
    args = parser.parse_args()
    prefix = Path(subprocess.check_output(["brew", "--prefix"], text=True).strip())
    brew_cache = Path(subprocess.check_output(["brew", "--cache"], text=True).strip())
    inputs = [*(brew_cache / "downloads").iterdir(), *args.archive_dir.iterdir()]
    for name in args.formulae:
        collect_formula((prefix / "opt" / name).resolve(strict=True), name, inputs, args.source)


if __name__ == "__main__":
    main()
