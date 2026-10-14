# ContactMic host bridge

Python 3.11+ receiver for the custom ContactMic BLE protocol, with separate
strict codecs, loss accounting, DC removal/gain, stateful 48 kHz resampling,
bounded live output, WAV recording and diagnostics.

CM-006 now attaches real acquisition to the firmware service. Sensor startup
failure still reports source unavailable, never synthetic audio. Physical
performance is a separate CM-007 gate. See `docs/CM-006-INTEGRATION.md` for current
session/fallback semantics; reconnect after a stopped AUTO boundary.

From the repository root, in a Python 3.11+ virtual environment:

```sh
python -m pip install './host[ble,audio]'
contactmic --help
contactmic discover
contactmic audio-devices
```

With integrated firmware and hardware, example operations are:

```sh
contactmic diagnostics --policy safe --duration 30
contactmic monitor --axis z --range 2 --policy safe --gain 4
contactmic record capture.wav --duration 30 --policy safe
```

No runtime extras are needed for offline tests:

```sh
python -m pip install ./host
python -m unittest discover -s host/tests -v
```

Use a non-editable install for verification; do not inject `host/src` into
`PYTHONPATH`. Tests consume `tests/vectors/v1/` from the repository and use fake
BLE/audio interfaces. Reinstall after editing source. No test accesses devices.

See `docs/CM-005-HOST.md` for installation, virtual-audio routing, diagnostics,
resource limits and explicit loss/recording semantics. This is a user-space
custom BLE bridge, not HFP, LE Audio or a kernel microphone driver.
