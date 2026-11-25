# Intel macOS performance baseline

Measured November 25, 2026 on the reference Intel Mac with the x86_64 Release workbench. The machine reported macOS 15.8.3 (24H314), a four-core 1.8 GHz Intel Core i5 with Hyper-Threading, 12 GB RAM, and Intel UHD 620 graphics. It was on AC power. The build used Apple Clang 17 through Command Line Tools, the macOS 15.5 SDK, Qt 6.12.0, CMake 4.5.0, Ninja 1.13.2, Scintilla 5.6.9, Lexilla 5.5.5, and QTermWidget 2.4.0.

Cocoa launch from the managed shell failed with `Cannot create window: no screens available`. The sweep used Qt's offscreen platform at scale 1. The measurements characterize the raster workbench in that environment. The on-screen Cocoa performance gates and foreground idle condition remain unverified.

| Gate | Proposed target | Offscreen result |
| --- | ---: | ---: |
| Fresh process launch to usable editor, median of five | ≤ 1.5 s | 0.549 s |
| Empty workspace editor RSS | ≤ 100 MiB | 41.21 MiB |
| Editor RSS with ten source tabs and one terminal | ≤ 160 MiB | 44.91 MiB |
| Idle editor CPU over 60 seconds | < 1% of one logical core | 0.0021% |
| Open a 1 MiB C++ file to an editable view, median of five | ≤ 500 ms | 40.35 ms |
| Key event to completed editor paint, p95 | ≤ 32 ms | 1.07 ms |

Launch samples in seconds were 0.553, 0.558, 0.549, 0.546, and 0.547. Open samples in milliseconds were 40.35, 43.16, 38.07, 41.91, and 37.27. All 50 keystrokes landed in the document. The 16 MiB C++ fixture opened and painted in 125.63 ms with syntax highlighting and clangd disabled by the large-file thresholds. Ten keystrokes in that file completed in 0.50–36.72 ms.

The first sweep retained the inherited `/bin/zsh` configuration. Its terminal stayed open and a sleeping `rm` remained under zsh throughout the sample, so prompt readiness was uncertain. At the start of that run, zsh used 1.95 MiB RSS, its child used 0.81 MiB, and Apple clangd 17 used 68.26 MiB. All three reported zero CPU time change over the measured window. The editor held 44.84 MiB RSS at the end. Git was inactive.

The sweep started after the Release build and CTest completed. The system's 1-, 5-, and 15-minute load averages were 6.43, 14.91, and 11.67 at the start and 2.97, 11.84, and 10.79 at the end. The recent compilation activity accounts for part of this background load.

## Reproduce

```sh
cmake --preset release
cmake --build --preset release --target nebula_bench
python3 scripts/benchmark.py
```

Use `--platform offscreen` if a display server is unavailable. The harness uses the Release workbench and its bundled resources through a standalone Qt Test executable. Each launch sample uses a fresh process with an empty editor and includes spawning the process. Each open sample uses a fresh workbench process and a generated 1 MiB UTF-8 C++ file of distinct function definitions. System file caches stay enabled.

The launch and open endpoints are the first completed editor viewport paint. Typing latency starts at Qt key injection and ends at the first completed viewport paint after a text-change signal. The harness checks the inserted text, paces the inputs 40 ms apart, and uses the nearest-rank p95 of 50 samples. Qt Test input and viewport paint timing omit physical keyboard, compositor, and display latency.

The empty and loaded RSS samples use separate fresh processes, measured after three seconds of settling. The loaded sample creates ten 4 KiB source tabs and starts the embedded terminal. macOS process resource accounting reports editor RSS and CPU separately from its shell and language-server descendants. The idle window runs the ordinary Qt event loop for at least 60 seconds, with no polling.
