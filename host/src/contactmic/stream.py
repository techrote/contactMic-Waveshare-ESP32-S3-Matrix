"""Session-local modular loss accounting, independent of listening concealment."""
from dataclasses import dataclass
from .protocol import AudioFrame, ProtocolError


@dataclass(slots=True)
class Diagnostics:
    accepted_packets: int = 0
    packet_gaps: int = 0
    source_gaps: int = 0
    duplicate_or_reordered: int = 0
    invalid_frames: int = 0
    source_discontinuity_packets: int = 0
    i2c_flag_packets: int = 0
    ring_flag_packets: int = 0
    concealed_samples: int = 0
    large_gap_resets: int = 0
    callback_queue_drops: int = 0


class StreamTracker:
    """Only forward modular deltas below half-range are accepted.

    Lifetime diagnostics are retained by reset; sequence and index state are not.
    The first observed packet's nonzero index counts source intervals absent from
    the start of the session. Packet gaps and source gaps may describe the same
    loss; never sum them or pretend an index gap alone identifies its cause.
    """
    def __init__(self, diagnostics: Diagnostics | None = None):
        self.diagnostics = diagnostics or Diagnostics()
        self.reset()

    def reset(self) -> None:
        self.last_sequence: int | None = None
        self.next_index: int | None = None
        self.metadata: tuple[int, int] | None = None

    def accept(self, frame: AudioFrame) -> int | None:
        metadata = frame.rate_millihz, frame.descriptor
        if self.metadata is not None and metadata != self.metadata:
            self.diagnostics.invalid_frames += 1
            raise ProtocolError("source metadata changed without a new session")
        if self.last_sequence is None:
            gap = frame.first_index
            packet_gap = frame.sequence
        else:
            seq_delta = (frame.sequence - self.last_sequence) & 0xffff
            gap = (frame.first_index - self.next_index) & 0xffffffff
            if not 0 < seq_delta < 0x8000 or gap >= 0x80000000:
                self.diagnostics.duplicate_or_reordered += 1
                return None
            packet_gap = seq_delta - 1
        self.metadata = metadata
        self.last_sequence = frame.sequence
        self.next_index = (frame.first_index + len(frame.samples)) & 0xffffffff
        d = self.diagnostics
        d.accepted_packets += 1
        d.packet_gaps += packet_gap
        d.source_gaps += gap
        d.source_discontinuity_packets += bool(frame.flags & 1)
        d.i2c_flag_packets += bool(frame.flags & 2)
        d.ring_flag_packets += bool(frame.flags & 4)
        return gap
