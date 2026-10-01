# ESP32 firmware CI checks

The quality workflow runs on main pushes, pull requests and manual runs with
read-only repository permissions. GitHub Actions use immutable commit pins;
Dependabot maintains the Actions updates.

PlatformIO builds the ESP32-S3 firmware and Cppcheck fails on high-severity
defects in first-party code. PlatformIO Core and its Python dependencies are
installed from the existing hash-locked, wheel-only requirements file.
CodeQL analyzes C/C++ and Python, and the sensitive-data workflow scans changes.

Pull requests check whitespace against their base and apply clang-format 18
only to changed first-party C/C++ lines under main and components. The checked-in
.clang-format sets LLVM style, four-space indentation and a 120-column limit.
Legacy formatting outside changed lines is not a new failure.

The size report captures firmware.bin immediately after each build, then builds
the PR base with the same runner and toolchain. It reports binary size growth
and flash/RAM usage in the job summary. Push and manual runs report the current
firmware size without a PR comparison. Temporary reports use the runner's
private temporary directory. No arbitrary size-growth threshold is imposed;
capacity errors remain enforced by the firmware build.

These changes affect development checks and reports only. Firmware sources,
runtime configuration and version numbers are unchanged; no release is needed.
