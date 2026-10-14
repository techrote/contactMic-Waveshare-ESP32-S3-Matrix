# ContactMic host package

CM-001 foundation only. The `contactmic` command exposes help and a development
version; BLE reception, DSP and audio output are **not implemented**. There are
no runtime dependencies or hardware accesses.

From the repository root, in a Python 3.11+ virtual environment:

```sh
python -m pip install ./host
python -m unittest discover -s host/tests -v
contactmic --help
python -m contactmic --version
```

The install is deliberately non-editable in verification: tests must exercise
the installed distribution, not a `PYTHONPATH` workaround. See
`docs/DEVELOPMENT.md` in the repository for full build/test instructions and
extension points. CM-005 will add the independent BLE, decoder, DSP and sink
layers without making imports or unit tests require physical devices.
