# Development and verification

## Current scope

CM-001 is a build/test foundation, not a working microphone. `app_main` logs one
explicit foundation message and returns; it does not initialize the sensor,
acquisition, BLE or LEDs. The host CLI offers help/version only. No physical
board, Bluetooth adapter or audio device is needed for the checks below.

Read [AGENTS.md](../AGENTS.md), [RAG.md](../RAG.md) and the active issue before
changing behavior. The accepted contracts and issue dependencies still apply.

## Layout and ownership

| Path | Boundary / next owning issue |
| --- | --- |
| `firmware/main/` | ESP-IDF app entry; integration belongs to CM-006 |
| `firmware/components/contactmic_board/` | Board/QMI register access, CM-002 |
| `firmware/components/contactmic_acquisition/` | DRDY/task/ring/rate policy, CM-003 |
| `firmware/components/contactmic_protocol/` | Pure wire encoding/control validation, CM-004 |
| `firmware/components/contactmic_ble/` | NimBLE transport/packetization, CM-004 |
| `host/src/contactmic/` | Installable Python host package, CM-005 |
| `host/tests/` | Standard-library unittest discovery for host behavior |
| `tests/native/` | CMake/CTest for actual pure firmware-core sources |
| `tests/tooling/` | Hardware-free build/toolchain guard tests |

The four firmware components are currently **interface-only** ESP-IDF components
(`idf_component_register(INCLUDE_DIRS "include")`). Their guarded headers reserve
include boundaries, not speculative runtime APIs. There are no pretend driver
success responses. Each owning issue adds its real sources to that component's
`SRCS`, public dependencies to `REQUIRES`, and implementation-only dependencies
to `PRIV_REQUIRES`. Add C++ linkage guards when public C declarations are added.

Keep hardware adapters out of native tests. Acquisition must not depend on BLE;
BLE must not write QMI registers. The app wires the accepted components together.

## Fresh checkout

```sh
git clone https://github.com/techrote/contactMic-Waveshare-ESP32-S3-Matrix.git
cd contactMic-Waveshare-ESP32-S3-Matrix
```

Use separate shells/environments for ESP-IDF's Python environment and the host
package virtual environment. Do not install host audio dependencies into IDF.

## Firmware: pinned ESP-IDF v5.5.5, target ESP32-S3

Install the prerequisites from Espressif's
[v5.5.5 Linux/macOS guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/linux-macos-setup.html),
then install this exact release outside the project checkout:

```sh
mkdir -p "$HOME/esp"
git clone --branch v5.5.5 --recursive https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf-v5.5.5"
cd "$HOME/esp/esp-idf-v5.5.5"
./install.sh esp32s3
. ./export.sh
```

Back at this repository root, in that activated shell:

```sh
idf.py --version
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
```

`--version` must identify v5.5.5. CMake rejects a different release or an explicitly
conflicting target, including one supplied through the environment. `set-target`
resets local configuration; for everyday incremental rebuilds use just `build`.
Generated `firmware/sdkconfig`, `sdkconfig.old` and `firmware/build/` are ignored;
commit intentional defaults to `sdkconfig.defaults`, not generated configuration.
No flash/PSRAM tuning or hardware qualification is implied by this skeleton.

Windows: install **v5.5.5** using Espressif's installer and use its ESP-IDF command
prompt/PowerShell environment, then run the same `idf.py` commands. Do not use an
unactivated ordinary shell or an Arduino toolchain.

Alternatively, with Docker available, from the repository root in Bash:

```sh
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.5.5 \
  /bin/bash -lc 'idf.py -C firmware set-target esp32s3 && idf.py -C firmware build'
```

The official [IDF Docker image](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/tools/idf-docker-image.html)
contains the cross-toolchain. CI uses this release tag and explicitly activates
`/opt/esp/idf/export.sh`. Building never flashes a board.

## Host install and tests

Use Python 3.11 or later. CI exercises 3.11 and 3.14 on Linux. No runtime packages
are required by the foundation; packaging downloads setuptools during installation.

Bash, from the repository root:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --no-cache-dir ./host
python -m pip check
python -m unittest discover -s host/tests -v
contactmic --help
python -m contactmic --version
```

PowerShell, from the repository root (substitute the installed Python version):

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --no-cache-dir ./host
.\.venv\Scripts\python.exe -m pip check
$env:PATH = "$PWD\.venv\Scripts;$env:PATH"
.\.venv\Scripts\python.exe -m unittest discover -s host/tests -v
.\.venv\Scripts\contactmic.exe --help
```

Verification uses a **non-editable install**. Do not inject `host/src` into
`PYTHONPATH`: CLI tests run the installed package from temporary directories in
isolated Python subprocesses. After source edits, reinstall before testing.
The development version is packaging scaffolding, not an accepted release.

For later CM-005 work, isolate BLE adapters, binary decoding, DSP and sinks in
separate modules. Imports must not access devices. Keep hardware libraries lazy
or injected so ordinary tests remain deterministic and hardware-free.

## Native/core and tooling tests

Use CMake 3.20+ (for the `ctest --test-dir` CLI), a C11 compiler and a C++17
compiler. From the repository root, outside the ESP-IDF build tree:

```sh
cmake -S tests/native -B build/native -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure --no-tests=error
python3 -m unittest discover -s tests/tooling -v
```

Use `python` instead of `python3` in the Windows environment. The native harness
currently has two C/C++ header-compilation smoke tests, **not sensor or protocol
tests**. Guard tests use fake `version.cmake` fixtures to exercise wrong-version,
missing-installation and wrong/conflicting-target errors; they are **not an
ESP-IDF build substitute**.

To add real core tests, add an executable in `tests/native/CMakeLists.txt`, compile
the relevant production `.c`/`.cpp` files, link the `contactmic_headers` interface
for public include paths, and register it with `add_test`. Do not clone production
logic into tests. CTest discovers registered tests without CI changes. Host and
tooling suites discover `test_*.py` with unittest. Keep native checks active in
Release builds too; do not rely solely on C `assert` compiled out by `NDEBUG`.

## CI and acceptance

`.github/workflows/ci.yml` runs on every PR update and on pushes to `main`. Feature
branch pushes do not create duplicate runs. Five checks run from clean checkouts:
workflow syntax (actionlint v1.7.7), firmware build, native/tooling tests, and host
installation/tests on each of the two Python versions. Workflow/step timeouts and
per-PR concurrency bound resource use. No artifact upload or persistent cache is
configured; generated files stay on disposable runners. Action revisions are
SHA-pinned. The firmware version is independently checked by CMake.

With Go installed, reproduce workflow syntax validation from the repository root:

```sh
go run github.com/rhysd/actionlint/cmd/actionlint@v1.7.7 -color
```

Do not equate a successful compile with physical performance acceptance. Merge
only after every required check passes on the current reviewed head. Monitor CI
at the mandatory **120-second minimum interval** from `AGENTS.md`; after merge,
verify `main` and record acceptance on the issue. Firmware and host streaming
behavior belong to later issues; physical measurements remain gated by CM-007.
