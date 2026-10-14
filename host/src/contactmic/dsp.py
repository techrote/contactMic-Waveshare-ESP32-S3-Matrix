"""Stateful source-domain processing and integer-phase linear resampling."""
from array import array
import math
from collections.abc import Iterable
from .protocol import AudioFrame, ProtocolError
from .stream import Diagnostics, StreamTracker

OUTPUT_RATE = 48000


def finite(value: float, name: str) -> float:
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{name} must be finite")
    return value


def valid_rate(rate_millihz: int) -> None:
    # DSP resource envelope, not a sensor ODR promise. Parser preserves all positive u32 rates.
    if not isinstance(rate_millihz, int) or not 100000 <= rate_millihz <= 48000000:
        raise ProtocolError("DSP source rate must be between 100 and 48000 Hz")


class DCBlocker:
    def __init__(self, rate_millihz: int, cutoff_hz: float = 20):
        valid_rate(rate_millihz)
        cutoff_hz = finite(cutoff_hz, "high-pass cutoff")
        if not 0 <= cutoff_hz < rate_millihz / 2000:
            raise ValueError("high-pass cutoff must be nonnegative and below source Nyquist")
        self.enabled = cutoff_hz > 0
        self.pole = math.exp(-2 * math.pi * cutoff_hz / (rate_millihz / 1000))
        self.reset()

    def reset(self) -> None:
        self.previous_input = 0.0
        self.previous_output = 0.0

    def sample(self, x: float) -> float:
        if not self.enabled:
            return x
        y = x - self.previous_input + self.pole * self.previous_output
        self.previous_input, self.previous_output = x, y
        return y


class LinearResampler:
    """Bounded state; rational milli-Hz phase prevents packet-dependent drift.

    One source sample of lookahead is required. No flush copies are manufactured.
    Each push is bounded to 1024 source samples; callers partition larger input.
    The result is float32 and invariant under arbitrary input packet partitioning.
    """
    def __init__(self, rate_millihz: int):
        valid_rate(rate_millihz)
        self.rate = rate_millihz
        self.reset()

    def reset(self) -> None:
        self.previous: float | None = None
        self.phase = 0

    def push(self, samples: Iterable[float]) -> array:
        batch = []
        for number, value in enumerate(samples):
            if number >= 1024:
                raise ValueError("resampler push exceeds 1024 source samples")
            value = finite(value, "sample")
            if not -1 <= value <= 1:
                raise ValueError("resampler samples must be within [-1, 1]")
            batch.append(value)
        out = array("f")
        for x in batch:
            if self.previous is None:
                self.previous = x
                continue
            while self.phase < OUTPUT_RATE * 1000:
                fraction = self.phase / (OUTPUT_RATE * 1000)
                out.append(self.previous + (x - self.previous) * fraction)
                self.phase += self.rate
            self.phase -= OUTPUT_RATE * 1000
            self.previous = x
        return out


class Pipeline:
    def __init__(self, *, gain: float = 1, cutoff_hz: float = 20,
                 conceal_ms: float = 5, raw: bool = False):
        self.gain = finite(gain, "gain")
        self.cutoff = finite(cutoff_hz, "high-pass cutoff")
        self.conceal_ms = finite(conceal_ms, "concealment interval")
        if not 0 <= self.gain <= 10000 or self.cutoff < 0 or not 0 <= self.conceal_ms <= 20:
            raise ValueError("gain must be 0..10000 and concealment 0..20 ms; cutoff must be nonnegative")
        self.raw = raw
        self.diagnostics = Diagnostics()
        self.tracker = StreamTracker(self.diagnostics)
        self.clipped_samples = 0
        self.reset()

    def reset(self) -> None:
        self.tracker.reset()
        self.blocker: DCBlocker | None = None
        self.resampler: LinearResampler | None = None
        self.previous_raw = 0.0
        self.have_previous = False

    def process(self, frame: AudioFrame) -> array:
        valid_rate(frame.rate_millihz)
        if not 1 <= len(frame.samples) <= 255 or any(
            not isinstance(x, int) or not -32768 <= x <= 32767 for x in frame.samples
        ):
            raise ProtocolError("invalid signed source samples")
        # Validate rate-dependent filter parameters before mutating sequence state.
        if self.blocker is None:
            blocker = DCBlocker(frame.rate_millihz, 0 if self.raw else self.cutoff)
            resampler = LinearResampler(frame.rate_millihz)
        else:
            blocker, resampler = self.blocker, self.resampler
        gap = self.tracker.accept(frame)
        if gap is None:
            return array("f")
        self.blocker, self.resampler = blocker, resampler
        limit = min(255, int(frame.rate_millihz * self.conceal_ms / 1000000))
        source: list[float] = []
        if gap:
            if gap <= limit and self.have_previous:
                source.extend([self.previous_raw] * gap)
                self.diagnostics.concealed_samples += gap
            else:
                # Do not allocate a gap-sized silence buffer or interpolate across stale history.
                self.blocker.reset()
                self.resampler.reset()
                self.diagnostics.large_gap_resets += 1
        source.extend(value / 32768.0 for value in frame.samples)
        self.previous_raw = source[-1]
        self.have_previous = True
        processed: list[float] = []
        for sample in source:
            value = sample if self.raw else self.blocker.sample(sample) * self.gain
            if not -1 <= value <= 1:
                self.clipped_samples += 1
            processed.append(max(-1.0, min(1.0, value)))
        return self.resampler.push(processed)
