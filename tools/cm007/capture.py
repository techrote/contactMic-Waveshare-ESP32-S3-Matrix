"""Serial, stimulus and subprocess-pipe capture for CM-007."""
from __future__ import annotations

from array import array
from dataclasses import dataclass
import importlib
import json
import math
from pathlib import Path
import queue
import threading
import time
from typing import Any

from .common import (
    SERIAL_RECORD_SCHEMA,
    STIMULUS_RECORD_SCHEMA,
    dbfs_amplitude,
    parse_device,
    parse_serial_line,
    profile_segment_detail,
    utc_now,
)


def optional_module(name: str, install_hint: str):
    try:
        return importlib.import_module(name)
    except (ImportError, OSError) as exc:
        raise RuntimeError(f"Missing optional dependency {name!r}; {install_hint}") from exc


@dataclass
class SerialSummary:
    port: str | None
    baud: int
    opened: bool = False
    error: str | None = None
    identity: dict[str, Any] | None = None
    identity_records: int = 0
    crash_markers: int = 0
    reset_markers: int = 0
    lines: int = 0


class SerialCapture(threading.Thread):
    def __init__(self, port: str, baud: int, path: Path, origin: float):
        super().__init__(name="cm007-serial", daemon=True)
        self.port = port
        self.baud = baud
        self.path = path
        self.origin = origin
        self.stop_event = threading.Event()
        self.open_event = threading.Event()
        self.identity_event = threading.Event()
        self.summary = SerialSummary(port=port, baud=baud)

    def stop(self) -> None:
        self.stop_event.set()

    def run(self) -> None:
        try:
            serial = optional_module(
                "serial", "install with: python -m pip install './host[qualification]'"
            )
            with serial.Serial(self.port, self.baud, timeout=0.2) as stream, self.path.open(
                "w", encoding="utf-8", newline="\n"
            ) as output:
                self.summary.opened = True
                self.open_event.set()
                while not self.stop_event.is_set():
                    raw = stream.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    parsed = parse_serial_line(line)
                    record = {
                        "schema": SERIAL_RECORD_SCHEMA,
                        "utc": utc_now(),
                        "elapsed_s": round(time.monotonic() - self.origin, 6),
                        "line": line,
                    }
                    output.write(json.dumps(record, sort_keys=True) + "\n")
                    output.flush()
                    self.summary.lines += 1
                    self.summary.crash_markers += len(parsed["crash_markers"])
                    self.summary.reset_markers += len(parsed["reset_markers"])
                    if parsed.get("identity") is not None:
                        self.summary.identity_records += 1
                        if self.summary.identity is None:
                            self.summary.identity = parsed["identity"]
                            self.identity_event.set()
        except Exception as exc:
            self.summary.error = f"{type(exc).__name__}: {exc}"
            self.open_event.set()


class StimulusPlayer(threading.Thread):
    def __init__(
        self, *, profile: str, duration: float, output_device: str | None,
        level_dbfs: float, channels: int, frequency: float, path: Path, origin: float,
    ):
        super().__init__(name="cm007-stimulus", daemon=True)
        self.profile = profile
        self.duration = duration
        self.output_device = parse_device(output_device)
        self.level_dbfs = level_dbfs
        self.channels = channels
        self.frequency = frequency
        self.path = path
        self.origin = origin
        self.stop_event = threading.Event()
        self.error: str | None = None

    def stop(self) -> None:
        self.stop_event.set()

    def run(self) -> None:
        try:
            sounddevice = optional_module(
                "sounddevice", "install with: python -m pip install './host[qualification]'"
            )
            amplitude = dbfs_amplitude(self.level_dbfs)
            sample_rate = 48000
            block = 480
            phase = 0.0
            last_segment: tuple[int, float | None] | None = None
            frame_cursor = 0
            total_frames = max(1, math.ceil(self.duration * sample_rate))
            with self.path.open("w", encoding="utf-8", newline="\n") as events, sounddevice.RawOutputStream(
                samplerate=sample_rate, channels=self.channels, dtype="float32",
                device=self.output_device, blocksize=block,
            ) as stream:
                while not self.stop_event.is_set() and frame_cursor < total_frames:
                    frames = min(block, total_frames - frame_cursor)
                    values = array("f")
                    for frame in range(frames):
                        stimulus_elapsed = (frame_cursor + frame) / sample_rate
                        index, hz, segment_elapsed, segment_duration = profile_segment_detail(
                            stimulus_elapsed, self.profile, self.frequency, self.duration
                        )
                        segment = (index, hz)
                        if segment != last_segment:
                            events.write(json.dumps({
                                "schema": STIMULUS_RECORD_SCHEMA,
                                "utc": utc_now(),
                                "elapsed_s": round(time.monotonic() - self.origin, 6),
                                "stimulus_elapsed_s": round(stimulus_elapsed, 6),
                                "segment": index,
                                "frequency_hz": hz,
                                "level_dbfs": self.level_dbfs,
                            }, sort_keys=True) + "\n")
                            events.flush()
                            last_segment = segment
                            if hz is not None:
                                phase = 0.0
                        if hz is None:
                            value = 0.0
                        else:
                            fade = 0.05
                            taper = min(
                                1.0,
                                segment_elapsed / fade,
                                max(0.0, (segment_duration - segment_elapsed) / fade),
                            )
                            value = amplitude * taper * math.sin(phase)
                            phase = (phase + 2.0 * math.pi * hz / sample_rate) % (2.0 * math.pi)
                        values.extend([value] * self.channels)
                    stream.write(values.tobytes())
                    frame_cursor += frames
        except Exception as exc:
            self.error = f"{type(exc).__name__}: {exc}"


class PipeReader(threading.Thread):
    def __init__(self, name: str, stream, messages: queue.Queue[tuple[str, str | None]]):
        super().__init__(name=f"cm007-{name}", daemon=True)
        self.label = name
        self.stream = stream
        self.messages = messages

    def run(self) -> None:
        try:
            for line in iter(self.stream.readline, ""):
                self.messages.put((self.label, line.rstrip("\r\n")))
        finally:
            self.messages.put((self.label, None))
