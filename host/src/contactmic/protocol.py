"""Strict, hardware-free codecs for CM-004 protocol major 1.

Wire definitions live in docs/CM-004-WIRE-V1.md. No device I/O occurs here.
"""
from dataclasses import dataclass
import struct

SERVICE_UUID = "7e400001-b5a3-f393-e0a9-e50e24dcca9e"
AUDIO_UUID = "7e400002-b5a3-f393-e0a9-e50e24dcca9e"
CONTROL_UUID = "7e400003-b5a3-f393-e0a9-e50e24dcca9e"
STATUS_UUID = "7e400004-b5a3-f393-e0a9-e50e24dcca9e"
NAME_PREFIX = "S3-ContactMic"
HEADER = struct.Struct("<BBHIIBBH")
POLICIES = {"safe": 0, "medium": 1, "high": 2, "auto": 3}
AXES = {"x": 0, "y": 1, "z": 2}


class ProtocolError(ValueError):
    """An unsupported, malformed or incoherent wire value."""


def descriptor_parts(value: int) -> tuple[int, int]:
    if value & ~7 or value & 3 == 3 or value < 0:
        raise ProtocolError("invalid axis/range descriptor")
    return value & 3, 4 if value & 4 else 2


@dataclass(frozen=True, slots=True)
class AudioFrame:
    sequence: int
    first_index: int
    rate_millihz: int
    flags: int
    descriptor: int
    samples: tuple[int, ...]


def decode_audio(data: bytes | bytearray | memoryview) -> AudioFrame:
    if len(data) < HEADER.size or len(data) > HEADER.size + 510:
        raise ProtocolError("audio frame length outside v1 bounds")
    major, flags, seq, index, rate, count, descriptor, reserved = HEADER.unpack_from(data)
    if major != 1:
        raise ProtocolError(f"unsupported audio protocol major {major}")
    if not count or len(data) != HEADER.size + count * 2:
        raise ProtocolError("audio sample count does not match length")
    if reserved or flags & ~7 or not rate:
        raise ProtocolError("invalid flags, reserved field or zero source rate")
    descriptor_parts(descriptor)
    samples = struct.unpack_from(f"<{count}h", data, HEADER.size)
    return AudioFrame(seq, index, rate, flags, descriptor, samples)


@dataclass(frozen=True, slots=True)
class Status:
    session_id: int
    rate_millihz: int
    mtu: int
    descriptor: int
    policy: int
    state: int
    selected_mode: int
    phy: int
    counters: tuple[int, ...] | None = None
    data_length: int = 0


def decode_status(data: bytes | bytearray | memoryview) -> Status:
    if len(data) not in (20, 64):
        raise ProtocolError("status must be a 20-byte event or 64-byte snapshot")
    major, kind, length, session, rate, mtu, desc, policy, state, mode, phy, reserved = (
        struct.unpack_from("<BBHIIHBBBBBB", data)
    )
    if major != 1 or kind not in (0, 1) or length != len(data) or (kind == 1) != (length == 20):
        raise ProtocolError("unsupported or malformed status header")
    descriptor_parts(desc)
    if reserved or policy > 3 or state > 2 or mode > 2 or phy > 3 or mtu < 23:
        raise ProtocolError("invalid status configuration")
    if state and (not session or not rate):
        raise ProtocolError("active status lacks session/rate")
    counters = None
    data_length = 0
    if kind == 0:
        counters = struct.unpack_from("<5Q", data, 20)
        data_length, tail = struct.unpack_from("<HH", data, 60)
        if tail:
            raise ProtocolError("nonzero status reserved tail")
    return Status(session, rate, mtu, desc, policy, state, mode, phy, counters, data_length)


def control_request(opcode: int, request_id: int, value: int | None = None) -> bytes:
    if not 1 <= opcode <= 9 or not 0 <= request_id <= 65535:
        raise ProtocolError("invalid control opcode or request id")
    payload = b""
    if opcode in (3, 4, 5):
        choices = range(3) if opcode == 3 else (2, 4) if opcode == 4 else range(4)
        if value not in choices:
            raise ProtocolError("invalid axis, range or policy")
        payload = bytes([value])
    elif opcode == 9:
        if value is None or not 1 <= value <= 0xffffffff:
            raise ProtocolError("commit-start requires the prepared session id")
        payload = struct.pack("<I", value)
    elif value is not None:
        raise ProtocolError("unexpected control payload")
    return struct.pack("<BBHHH", 1, opcode, request_id, len(payload), 0) + payload


def control_response(data: bytes, opcode: int, request_id: int) -> tuple[int, bytes]:
    if not 8 <= len(data) <= 72:
        raise ProtocolError("invalid control response length")
    major, op, ident, result, reserved, size = struct.unpack_from("<BBHBBH", data)
    if major != 1 or op != (opcode | 0x80) or ident != request_id:
        raise ProtocolError("control response identity mismatch")
    if reserved or result > 6 or len(data) != 8 + size:
        raise ProtocolError("invalid control response fields")
    return result, data[8:]
