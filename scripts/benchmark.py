#!/usr/bin/env python3
"""Measure the Release workbench on a local Mac."""
import argparse
import json
import math
import os
from pathlib import Path
import select
import statistics
import subprocess
import tempfile
import time


def launch_probe(command):
    with tempfile.TemporaryFile() as errors:
        start = time.monotonic()
        with subprocess.Popen([*command, "--launch"], stdout=subprocess.PIPE, stderr=errors) as proc:
            try:
                if not select.select([proc.stdout], [], [], 30)[0]:
                    raise RuntimeError("Timed out waiting for the first editor paint")
                ready = proc.stdout.readline() == b"ready\n"
                elapsed = time.monotonic() - start
                returncode = proc.wait(timeout=10)
                if not ready or returncode:
                    errors.seek(0)
                    raise RuntimeError(f"Launch probe failed ({returncode})\n{errors.read().decode()}")
                return elapsed
            finally:
                if proc.poll() is None:
                    proc.kill()


def probe(command, *arguments, timeout=30):
    result = subprocess.run([*command, *arguments], capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{arguments[0]} failed ({result.returncode})\n{result.stderr}")
    return json.loads(result.stdout.splitlines()[-1])


def percentile(samples, fraction):
    return sorted(samples)[math.ceil(fraction * len(samples)) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=Path("build/release/nebula_bench"))
    parser.add_argument("--platform", choices=["cocoa", "offscreen"], default="cocoa")
    parser.add_argument("--idle-seconds", type=int, default=60)
    parser.add_argument("--verify-prompt", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.idle_seconds <= 600:
        parser.error("--idle-seconds must be between 1 and 600")
    command = [args.binary.resolve(), "-platform", args.platform]

    before_load = os.getloadavg()
    launches = [launch_probe(command) for _ in range(5)]
    opens = [probe(command, "--open") for _ in range(5)]
    empty = probe(command, "--empty")
    typing = probe(command, "--typing")
    idle_args = ["--idle", str(args.idle_seconds)]
    if args.verify_prompt:
        idle_args.append("--verify-prompt")
    idle = probe(command, *idle_args, timeout=args.idle_seconds + 30)
    large = probe(command, "--large")
    platform_scale = {(sample["platform"], sample["scale"]) for sample in [*opens, empty, typing, idle, large]}
    if len(platform_scale) != 1:
        raise RuntimeError(f"Mixed platform or display scales in one run: {platform_scale}")
    platform, scale = platform_scale.pop()

    open_times = [sample["open_ms"] for sample in opens]
    result = {
        "platform": platform,
        "scale": scale,
        "system_load_average_before": before_load,
        "system_load_average_after": os.getloadavg(),
        "launch_seconds": launches,
        "launch_median_s": statistics.median(launches),
        "open_ms": open_times,
        "open_median_ms": statistics.median(open_times),
        "typing_ms": typing["typing_ms"],
        "typing_p95_ms": percentile(typing["typing_ms"], .95),
        "typing_active_window": typing["active_window"],
        "empty_rss_mib": empty["empty_rss_mib"],
        "loaded_rss_mib": idle["loaded_rss_mib"],
        "loaded_rss_end_mib": idle["loaded_rss_end_mib"],
        "idle_elapsed_seconds": idle["idle_elapsed_seconds"],
        "idle_cpu_percent": idle["idle_cpu_percent"],
        "stayed_foreground": platform == "cocoa" and idle["stayed_foreground"],
        "terminal_open_at_end": idle["terminal_open_at_end"],
        "terminal_prompt_verified": idle.get("terminal_prompt_verified", False),
        "children": idle["children"],
        "large_open_ms": large["open_ms"],
        "large_typing_ms": large["typing_ms"],
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
