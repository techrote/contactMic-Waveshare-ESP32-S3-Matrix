# Development and verification

## Current scope and ownership

Read [AGENTS.md](../AGENTS.md), [RAG.md](../RAG.md) and the active issue first.
The driver, BLE transport and host bridge now have implementations. Real DRDY
acquisition and end-to-end firmware wiring remain CM-003/CM-006; physical
qualification remains CM-007. `app_main` advertises the custom service with no
source attached and explicitly rejects start requests. It does not initialize
the QMI sensor or LEDs. Offline tests need no attached hardware.

| Path | Responsibility |
| --- | --- |
| `firmware/main/` | App composition; real source integration is CM-006 |
| `firmware/components/contactmic_board/` | QMI policy and ESP-IDF I2C adapter |
| `firmware/components/contactmic_acquisition/` | Reserved CM-003 DRDY/ring boundary |
| `firmware/components/contactmic_protocol/` | Pure v1 wire codecs |
| `firmware/components/contactmic_ble/` | Source-independent transport and NimBLE adapter |
| `host/src/contactmic/` | Protocol, stream accounting, DSP, audio, BLE receiver and CLI |
| `tests/native/` | Production-core C/C++ tests and header smoke tests |
| `tests/tooling/` | Toolchain guard rejection paths |
| `tests/vectors/v1/` | Shared firmware/host golden wire bytes |
| `host/tests/` | Installed-package tests with fake BLE/audio interfaces |

Keep hardware I/O out of native tests, acquisition out of BLE callbacks and
imports free of device access. Driver/transport lifecycle details are in
[CM-002-DRIVER.md](CM-002-DRIVER.md) and [CM-004-WIRE-V1.md](CM-004-WIRE-V1.md).

## Fresh checkout and firmware

```sh
git clone https://github.com/techrote/contactMic-Waveshare-ESP32-S3-Matrix.git
cd contactMic-Waveshare-ESP32-S3-Matrix
```

Install Espressif's platform prerequisites from its pinned
[v5.5.5 guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/linux-macos-setup.html).
Install the exact toolchain outside this repository:

```sh
mkdir -p "$HOME/esp"
git clone --branch v5.5.5 --recursive https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf-v5.5.5"
cd "$HOME/esp/esp-idf-v5.5.5"
./install.sh esp32s3
. ./export.sh
```

Return to the repository root in that activated shell:

```sh
idf.py --version
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
```

CMake requires **ESP-IDF v5.5.5 / ESP32-S3** and rejects conflicting version/target
inputs. `set-target` resets local configuration; ordinary incremental builds use
only `build`. Generated `firmware/build/`, `sdkconfig` and `sdkconfig.old` stay
untracked. Intentional defaults belong in `sdkconfig.defaults`. Do not replace
ESP-IDF with Arduino. Windows uses the v5.5.5 ESP-IDF installer/environment and
the same commands, not an ordinary unactivated PowerShell session.

Alternatively, from the repository root with Docker in Bash:

```sh
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.5.5 \
  /bin/bash -c 'idf.py -C firmware set-target esp32s3 && idf.py -C firmware build'
```

Use a non-login shell so the image's activated toolchain PATH is retained.
Building does not flash a board. Do not infer hardware qualification from a build.

## Host install and tests

Use a separate Python 3.11+ environment from ESP-IDF:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --no-cache-dir './host[ble,audio]'
python -m pip check
python -m unittest discover -s host/tests -v
contactmic --help
```

The base `./host` install also runs offline tests without optional runtime
libraries. Verification uses a non-editable installed wheel/package, not a
`PYTHONPATH` injection. Reinstall after source changes. Windows setup, CLI usage,
virtual audio routing and recording semantics are in [CM-005-HOST.md](CM-005-HOST.md).
The development version is not a hardware-qualified release.

## Native/core and tooling tests

Use CMake 3.20+ (for `ctest --test-dir`), C11 and C++17 compilers:

```sh
cmake -S tests/native -B build/native -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure --no-tests=error
python3 -m unittest discover -s tests/tooling -v
```

Tests compile real production sources, not replacement implementations. Add new
executables and `add_test` entries in the native CMake file; link
`contactmic_headers` for shared include paths. Assertions must remain active in
Release builds. Python suites discover `test_*.py` through unittest. Toolchain
guards use fixtures, not a substitute ESP-IDF build.

## CI

Every PR update and push to main runs seven jobs: workflow validation, pinned
firmware build, native/tooling tests, and host package/dependency/tests on Linux
and Windows with Python 3.11 and 3.14. Feature pushes do not duplicate PR runs.
Checks have job timeouts, read-only permissions and per-PR concurrency. Actions
are SHA-pinned; there are no routine artifact uploads or persistent caches.

With Go installed, reproduce workflow validation with:

```sh
go run github.com/rhysd/actionlint/cmd/actionlint@v1.7.7 -color
```

Follow AGENTS.md: inspect CI no more frequently than once per 120 seconds while
pending/running, review the current tested head, merge only on complete green
gates, and verify the exact default-branch result and its CI before acceptance.
