#!/usr/bin/env python3
"""Compare Nebula and VS Code on the same Linux X11 display."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import platform
import re
import signal
import statistics
import subprocess
import tempfile
import time


def process_tree_pss(root):
    children = {}
    for row in subprocess.check_output(["ps", "-e", "-o", "pid=,ppid="], text=True).splitlines():
        pid, parent = map(int, row.split())
        children.setdefault(parent, []).append(pid)

    pending = [root]
    kib = 0
    while pending:
        pid = pending.pop()
        pending.extend(children.get(pid, []))
        try:
            rows = Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
        except PermissionError:
            rows = subprocess.check_output(
                ["sudo", "-n", "cat", f"/proc/{pid}/smaps_rollup"], text=True).splitlines()
        except (FileNotFoundError, ProcessLookupError):
            if pid == root:
                raise
            continue
        kib += int(next(row.split()[1] for row in rows if row.startswith("Pss:")))
    return kib / 1024


@contextmanager
def editor_process(command, env, log):
    with subprocess.Popen(command, env=env, stdout=log, stderr=log, start_new_session=True) as proc:
        try:
            yield proc
        finally:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=5)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=5)


def wait_for_document_window(proc, document, start):
    # xdotool search --sync polls at a coarse interval that masks fast launches.
    command = ["xdotool", "search", "--all", "--onlyvisible", "--pid", str(proc.pid),
               "--name", re.escape(document.name)]
    while time.monotonic() - start < 35:
        if proc.poll() is not None:
            raise OSError(f"editor exited with status {proc.returncode}")
        search = subprocess.run(command, stdout=subprocess.DEVNULL, timeout=2)
        if search.returncode == 0:
            return (time.monotonic() - start) * 1000
        time.sleep(0.01)
    raise TimeoutError("the document window did not appear within 35 seconds")


def sample(binary, editor, document, settle):
    with tempfile.TemporaryDirectory(prefix=f"editor-bench-{editor}-") as directory:
        profile = Path(directory)
        config = profile / "config"
        config.mkdir()
        env = dict(os.environ, XDG_CONFIG_HOME=str(config), XDG_DATA_HOME=str(profile / "data"),
                   XDG_STATE_HOME=str(profile / "state"), XDG_CACHE_HOME=str(profile / "cache"))
        command = [str(binary)]
        if editor == "vscode":
            settings = config / "User" / "settings.json"
            settings.parent.mkdir()
            settings.write_text(json.dumps({
                "telemetry.telemetryLevel": "off",
                "update.mode": "none",
                "workbench.startupEditor": "none",
            }), encoding="utf-8")
            command.extend(["--disable-extensions", "--new-window", "--user-data-dir", str(config),
                            "--extensions-dir", str(profile / "extensions")])
        command.append(str(document))

        with tempfile.TemporaryFile(mode="w+b") as log:
            start = time.monotonic()
            with editor_process(command, env, log) as proc:
                try:
                    visible_ms = wait_for_document_window(proc, document, start)
                    time.sleep(settle)
                    pss = process_tree_pss(proc.pid)
                except (subprocess.SubprocessError, OSError) as error:
                    log.seek(0)
                    raise RuntimeError(f"{editor} failed on {document.name}: "
                                       f"{log.read().decode(errors='replace')}") from error
        return round(visible_ms, 2), round(pss, 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nebula", type=Path)
    parser.add_argument("vscode", type=Path, help="Path to VS Code's GUI executable")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--settle", type=float, default=5, help="Seconds before measuring PSS")
    args = parser.parse_args()
    if platform.system() != "Linux" or not os.environ.get("DISPLAY"):
        parser.error("a Linux X11 display is required")
    if args.runs < 3 or args.settle <= 0:
        parser.error("use at least three runs and a positive settle time")
    editors = {"nebula": args.nebula.resolve(), "vscode": args.vscode.resolve()}
    vscode_app = editors["vscode"].parent / "resources" / "app"
    vscode_package = json.loads((vscode_app / "package.json").read_text())
    vscode_product = json.loads((vscode_app / "product.json").read_text())
    vscode_build = {"version": vscode_package["version"], "commit": vscode_product.get("commit")}
    nebula_revision = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    cpu = next((line.partition(":")[2].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.machine())
    result = {"system": platform.platform(), "cpu": cpu, "logical_cpus": os.cpu_count(),
              "nebula_revision": nebula_revision, "vscode_build": vscode_build,
              "runs_per_editor": args.runs, "pss_settle_seconds": args.settle, "workloads": {}}

    with tempfile.TemporaryDirectory(prefix="editor-bench-input-") as directory:
        for name, lines in [("small", 1_000), ("large", 150_000)]:
            document = Path(directory) / f"nebula-bench-{name}.txt"
            document.write_bytes(b"Nebula editor benchmark: identical plain-text input for both applications.\n" * lines)
            samples = {editor: [] for editor in editors}
            for index in range(args.runs):
                order = list(editors)
                if index % 2:
                    order.reverse()
                for editor in order:
                    samples[editor].append(sample(editors[editor], editor, document, args.settle))
            workload = {"bytes": document.stat().st_size}
            for editor, values in samples.items():
                latency, memory = map(list, zip(*values))
                workload[editor] = {
                    "visible_ms_samples": latency,
                    "visible_ms_median": statistics.median(latency),
                    "pss_mib_samples": memory,
                    "pss_mib_median": statistics.median(memory),
                }
            workload["vscode_to_nebula_time_ratio"] = round(
                workload["vscode"]["visible_ms_median"] / workload["nebula"]["visible_ms_median"], 2)
            workload["vscode_to_nebula_pss_ratio"] = round(
                workload["vscode"]["pss_mib_median"] / workload["nebula"]["pss_mib_median"], 2)
            result["workloads"][name] = workload
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
