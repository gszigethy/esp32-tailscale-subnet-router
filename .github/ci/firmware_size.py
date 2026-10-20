"""Report built firmware growth without an arbitrary regression threshold."""
import os
from pathlib import Path

size_dir = Path(os.environ["SIZE_DIR"])
head = int((size_dir / "head-bytes.txt").read_text())
lines = ["## ESP32-S3 firmware size", "", f"Current firmware binary: **{head:,} bytes**."]
base_path = size_dir / "base-bytes.txt"
if base_path.exists():
    base = int(base_path.read_text())
    delta = head - base
    lines += [f"PR base: **{base:,} bytes**; change: **{delta:+,} bytes** ({delta / base * 100:+.2f}%)."]
for label, file in [("Current flash/RAM", "head-size.txt"), ("Base flash/RAM", "base-size.txt")]:
    path = size_dir / file
    if path.exists():
        values = [line for line in path.read_text().splitlines() if "RAM:" in line or "Flash:" in line]
        lines += ["", label, "```text", *values, "```"]
report = "\n".join(lines) + "\n"
print(report)
with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as output:
    output.write(report)
