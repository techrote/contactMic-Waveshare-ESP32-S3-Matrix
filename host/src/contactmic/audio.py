"""Bounded float32 FIFO, PCM WAV and lazily opened PortAudio output."""
from array import array
from collections import deque
from pathlib import Path
import math
import struct
import threading
import wave
from collections.abc import Iterable

from .dsp import OUTPUT_RATE


class AudioError(RuntimeError):
    """Actionable audio setup or output failure."""


class AudioFIFO:
    def __init__(self, capacity: int = 4800):
        if not isinstance(capacity, int) or not 1 <= capacity <= OUTPUT_RATE:
            raise ValueError("audio capacity must be 1..48000 samples")
        self.capacity = capacity
        self._samples: deque[float] = deque()
        self._lock = threading.Lock()
        self.overrun_samples = 0
        self.underrun_samples = 0
        self.contention_underruns = 0

    def clear(self) -> None:
        with self._lock:
            self._samples.clear()

    def __len__(self) -> int:
        with self._lock:
            return len(self._samples)

    def write(self, samples: Iterable[float]) -> None:
        # Convert/validate outside the callback lock; cap each application batch.
        values = []
        for count, value in enumerate(samples):
            if count >= 262144:
                raise ValueError("audio write exceeds bounded batch size")
            value = float(value)
            if not math.isfinite(value) or not -1 <= value <= 1:
                raise ValueError("audio samples must be finite and within [-1, 1]")
            values.append(value)
        if not values:
            return
        with self._lock:
            drop = max(0, len(self._samples) + len(values) - self.capacity)
            self.overrun_samples += drop
            if len(values) >= self.capacity:
                self._samples.clear()
                self._samples.extend(values[-self.capacity:])
            else:
                for _ in range(drop):
                    self._samples.popleft()
                self._samples.extend(values)

    def read(self, count: int) -> array:
        if not isinstance(count, int) or not 0 <= count <= OUTPUT_RATE:
            raise ValueError("audio callback count outside bounded range")
        output = array("f", [0]) * count
        # PortAudio callback never waits for a producer-held lock.
        if not self._lock.acquire(blocking=False):
            self.underrun_samples += count
            self.contention_underruns += 1
            return output
        try:
            available = min(count, len(self._samples))
            for index in range(available):
                output[index] = self._samples.popleft()
            self.underrun_samples += count - available
        finally:
            self._lock.release()
        return output


class WavSink:
    """48 kHz, mono, little-endian signed PCM16; saturates and quantizes float32."""
    def __init__(self, path: str | Path, *, overwrite: bool = False):
        try:
            self._handle = open(path, "wb" if overwrite else "xb")
        except OSError as exc:
            raise AudioError(f"Cannot create WAV {str(path)!r}: {exc}; use --overwrite only to replace it") from exc
        try:
            self._file = wave.open(self._handle, "wb")
        except Exception:
            self._handle.close()
            raise
        self._file.setnchannels(1)
        self._file.setsampwidth(2)
        self._file.setframerate(OUTPUT_RATE)
        self._closed = False

    def write(self, samples: Iterable[float]) -> None:
        if self._closed:
            raise AudioError("WAV sink is closed")
        pcm = bytearray()
        for count, value in enumerate(samples):
            if count >= 262144:
                raise ValueError("WAV write exceeds bounded batch size")
            value = float(value)
            if not math.isfinite(value):
                raise ValueError("WAV samples must be finite")
            integer = max(-32768, min(32767, round(value * 32768)))
            pcm.extend(struct.pack("<h", integer))
        self._file.writeframesraw(pcm)

    def close(self) -> None:
        if not self._closed:
            try:
                self._file.close()
            finally:
                self._handle.close()
                self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def sounddevice_module():
    try:
        import sounddevice
    except (ImportError, OSError) as exc:
        raise AudioError("Live audio needs the 'audio' extra and a working PortAudio installation; "
                         "install with: python -m pip install './host[audio]'") from exc
    return sounddevice


def list_audio_devices(backend=None):
    backend = backend or sounddevice_module()
    try:
        return backend.query_devices()
    except Exception as exc:
        raise AudioError(f"Cannot enumerate audio devices: {exc}") from exc


class LiveSink:
    def __init__(self, fifo: AudioFIFO, device=None, *, backend=None):
        self.fifo = fifo
        self.backend = backend or sounddevice_module()
        self.callback_failures = 0
        self.backend_underflows = 0
        self._stream = None
        try:
            self._stream = self.backend.RawOutputStream(
                samplerate=OUTPUT_RATE, channels=1, dtype="float32", device=device,
                blocksize=480, callback=self._callback,
            )
            self._stream.start()
        except Exception as exc:
            if self._stream is not None:
                try:
                    self._stream.close()
                except Exception:
                    pass
            raise AudioError(f"Cannot open 48 kHz mono audio output {device!r}: {exc}") from exc

    def _callback(self, outdata, frames, _time, status) -> None:
        if getattr(status, "output_underflow", False):
            self.backend_underflows += 1
        try:
            outdata[:] = self.fifo.read(frames).tobytes()
        except Exception:
            self.callback_failures += 1
            outdata[:] = b"\x00" * len(outdata)

    def write(self, samples: Iterable[float]) -> None:
        self.fifo.write(samples)

    def close(self) -> None:
        if self._stream is not None:
            try:
                self._stream.stop()
            finally:
                self._stream.close()
                self._stream = None
