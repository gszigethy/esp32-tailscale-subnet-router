#!/usr/bin/env python3
"""Run DDNS network-boundary regressions with the installed ESP-IDF HTTP parser.

Requires a C compiler and the PlatformIO framework package (installed by pio run).
No device or external network is used. AddressSanitizer checks late DNS completion.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    core_dir = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    parser = core_dir / "packages/framework-espidf/components/http_parser"
    if not (parser / "http_parser.c").is_file():
        raise SystemExit("ESP-IDF HTTP parser missing; run pio run -e esp32-s3 first")
    tests = ROOT / "tools/ddns_host_test"
    with tempfile.TemporaryDirectory(prefix="ddns-http-") as tmp:
        tmp = Path(tmp)
        headers = ["esp_app_desc.h", "esp_crt_bundle.h", "esp_heap_caps.h", "esp_timer.h", "esp_tls.h",
                   "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h",
                   "lwip/dns.h", "lwip/inet.h", "lwip/tcpip.h"]
        for name in headers:
            dst = tmp / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text('#include "http_shim.h"\n')
        binary = tmp / "test_ddns_http"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        f"-I{tmp}", f"-I{tests}", f"-I{ROOT / 'components/ddns'}", f"-I{parser}",
                        str(tests / "test_ddns_http.c"), str(ROOT / "components/ddns/ddns_http.c"),
                        str(ROOT / "components/ddns/ddns_core.c"), str(parser / "http_parser.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
