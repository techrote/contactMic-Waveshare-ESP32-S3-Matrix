"""USB-only CM-009 capture and conservative evidence evaluation.

No device imports or access occur at module import. Audio is captured from the
native OS UAC input, never from BLE or serial PCM. CDC is a correlated side channel.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import math
from pathlib import Path
import platform
import queue
import re
import sys
import threading
import time
import wave
from typing import Any

SCHEMA = "cm009.v1"
SOURCE_KEYS = ("intervals", "reads", "missed", "i2c", "overruns", "faults")
USB_KEYS = ("output_samples", "startup_silence", "starvation_silence", "failed_silence",
            "source_gaps", "gap_zeros", "duplicate_source", "large_gap_resets", "dsp_clips",
            "raw_clips", "resampler_resets", "queue_discards", "clock_updates", "clock_saturated",
            "short_writes", "late_frames", "transitions", "rejected_controls", "cdc_overflows",
            "cdc_dropped_replies")
HOST_KEYS = ("frames", "callbacks", "overflows", "queue_drops", "bad_blocks")
IDENTITY_KEYS = ("serial", "boot_id", "firmware", "elf_sha256", "generation")
MAX_ROW_BYTES = 16384
MAX_ROWS = 8000
MAX_TELEMETRY_GAP_US = 3500000


class QualificationError(RuntimeError):
    """A setup, capture, identity or evidence-integrity failure."""


def integer(value: Any, name: str, maximum: int = (1 << 64) - 1) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise QualificationError(f"{name} is not an unsigned integer in range")
    return value


def validate_status(data: Any) -> dict:
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise QualificationError("unsupported CDC status schema")
    if not re.fullmatch(r"[0-9A-F]{12}", str(data.get("serial", ""))):
        raise QualificationError("missing stable USB serial")
    if not re.fullmatch(r"[0-9a-f]{40}", str(data.get("firmware", ""))):
        raise QualificationError("firmware Git identity unavailable")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", str(data.get("elf_sha256", ""))):
        raise QualificationError("firmware ELF hash unavailable")
    for key in ("request_id", "result", "boot_id", "reset_reason", "generation", "state", "reason", "mode",
                "source_rate_millihz", "axis", "range_g", "policy", "gain_milli", "cutoff_millihz",
                "uptime_us", "output_position", "queue", "queue_high", "queue_low"):
        integer(data.get(key), key)
    if data["result"] or data["state"] > 4 or data["mode"] > 2 or data["policy"] > 3:
        raise QualificationError("firmware rejected request or returned invalid state/mode")
    if data.get("uac_rate") != 48000 or data["axis"] > 2 or data["range_g"] not in (2, 4):
        raise QualificationError("invalid source or USB format metadata")
    if data["queue"] > 256 or data["queue_high"] > 256:
        raise QualificationError("source queue exceeded its declared bound")
    for group, keys in (("source", SOURCE_KEYS), ("usb", USB_KEYS)):
        counters = data.get(group)
        if not isinstance(counters, dict):
            raise QualificationError(f"missing {group} counters")
        for key in keys:
            integer(counters.get(key), f"{group}.{key}")
    qmi = data.get("qmi")
    if data.get("source_available") is not True or not isinstance(qmi, dict):
        raise QualificationError("physical QMI source unavailable")
    if qmi.get("address") not in (0x6a, 0x6b) or qmi.get("who") != 5:
        raise QualificationError("unexpected QMI identity")
    integer(qmi.get("revision"), "QMI revision", 255)
    return data


def choose_interfaces(serial_id: str, ports: list, devices: list, hostapis: list,
                      port: str | None = None, audio_index: int | None = None) -> tuple[Any, int]:
    """Product strings carry the complete USB serial; truncated names fail closed.

    OS names alone are not cryptographic identity. The CDC query is checked again
    before capture, and exact endpoint/port metadata is retained in evidence.
    """
    if not re.fullmatch(r"[0-9A-F]{12}", serial_id):
        raise QualificationError("--serial must be the complete 12-digit uppercase device serial")
    matching_ports = [p for p in ports if getattr(p, "serial_number", None) == serial_id
                      and getattr(p, "vid", None) == 0xcafe and getattr(p, "pid", None) == 0x4014
                      and (port is None or p.device == port)]
    candidates = []
    for index, device in enumerate(devices):
        name = str(device.get("name", ""))
        identity_present = re.search(r"(?<![0-9A-F])" + re.escape(serial_id) + r"(?![0-9A-F])", name)
        if device.get("max_input_channels", 0) < 1 or not identity_present or "CM USB " not in name:
            continue
        if audio_index is not None and index != audio_index:
            continue
        # Prefer the native Windows WASAPI endpoint, not aliases from MME/DS.
        api_index = device.get("hostapi")
        api = hostapis[api_index]["name"] if type(api_index) is int and 0 <= api_index < len(hostapis) else ""
        candidates.append((index, api))
    wasapi = [index for index, api in candidates if "WASAPI" in api.upper()]
    chosen = wasapi if wasapi else [index for index, _ in candidates]
    if len(matching_ports) != 1 or len(chosen) != 1:
        raise QualificationError("ambiguous/missing same-board CDC and microphone; use devices and exact --port/--audio-device")
    return matching_ports[0], chosen[0]


class ControlLink:
    def __init__(self, serial_object, serial_id: str, timeout: float = 2.0):
        self.serial = serial_object
        self.serial_id = serial_id
        self.timeout = timeout
        self.ident = 0

    def request(self, command: str = "STATUS", values: tuple[int, ...] = ()) -> dict:
        if command not in ("STATUS", "CONFIG") or (command == "STATUS" and values) or (
                command == "CONFIG" and len(values) != 5):
            raise ValueError("unsupported control request")
        self.ident += 1
        if self.ident > 0xffffffff:
            raise QualificationError("control request identity exhausted")
        suffix = "".join(f" {integer(v, 'configuration', 0xffffffff)}" for v in values)
        request = f"{command} {self.ident}{suffix}\n".encode("ascii")
        if self.serial.write(request) != len(request):
            raise QualificationError("short CDC command write")
        deadline = time.monotonic() + self.timeout
        collected = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read_until(b"\n", size=4096 - len(collected))
            collected.extend(chunk)
            if len(collected) >= 4096:
                raise QualificationError("CDC response exceeds bounded size")
            if collected.endswith(b"\n"):
                try:
                    status = validate_status(json.loads(collected))
                except (ValueError, UnicodeError) as exc:
                    raise QualificationError(f"malformed CDC response: {exc}") from exc
                if status["request_id"] != self.ident or status["serial"] != self.serial_id:
                    raise QualificationError("CDC reply identity mismatch")
                return status
        raise QualificationError("CDC telemetry timeout; missing panic text is not proof of no reset")


@dataclass(frozen=True)
class CaptureCounts:
    frames: int = 0
    callbacks: int = 0
    overflows: int = 0
    queue_drops: int = 0
    bad_blocks: int = 0


class CaptureBuffer:
    """One PortAudio callback writer; immutable counter snapshots and bounded ingress."""
    def __init__(self, save_audio: bool, capacity: int = 64):
        self.counts = CaptureCounts()
        self.queue: queue.Queue[bytes] = queue.Queue(maxsize=capacity)
        self.save_audio = save_audio

    def callback(self, data, frames, _clock, status):
        old = self.counts
        overflow = int(bool(getattr(status, "input_overflow", False)))
        bad = type(frames) is not int or not 0 < frames <= 2048 or len(data) != frames * 2
        drop = 0
        if not bad and self.save_audio:
            try:
                self.queue.put_nowait(bytes(data))
            except queue.Full:
                drop = 1
        self.counts = CaptureCounts(old.frames + (0 if bad else frames), old.callbacks + 1,
            old.overflows + overflow, old.queue_drops + drop, old.bad_blocks + int(bad))


class WaveWriter:
    def __init__(self, buffer: CaptureBuffer, path: Path):
        self.buffer = buffer
        self.stop_event = threading.Event()
        self.error: str | None = None
        self.path = path
        self.thread = threading.Thread(target=self._run, name="cm009-wav", daemon=True)
        self.thread.start()

    def _run(self):
        try:
            with self.path.open("xb") as handle, wave.open(handle, "wb") as wav:
                wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(48000)
                while not self.stop_event.is_set() or not self.buffer.queue.empty():
                    try:
                        block = self.buffer.queue.get(timeout=0.1)
                    except queue.Empty:
                        continue
                    wav.writeframesraw(block)
        except Exception as exc:
            self.error = f"WAV writer: {exc}"

    def close(self):
        self.stop_event.set()
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            self.error = "WAV writer did not terminate; evidence incomplete"


def evaluate(metadata: dict, rows: list[dict]) -> dict:
    """Run-level evidence only: never closes the hardware/lifecycle/contact gate."""
    result: dict = {"schema": "cm009.summary.v1", "result": "incomplete", "issue_accepted": False,
        "failures": [], "review": [], "physical_checks_remaining": ["ordinary-app capture", "recovery",
        "close/reopen and cable cycles", "suspend/resume", "axis/range and direct-contact examples"]}
    try:
        if not isinstance(metadata, dict) or not isinstance(rows, list) or any(not isinstance(r, dict) for r in rows):
            raise QualificationError("invalid metadata/telemetry structure")
        if metadata.get("schema") != "cm009.capture.v1":
            raise QualificationError("unknown capture metadata schema")
        if metadata.get("error") or metadata.get("cleanup_errors"):
            raise QualificationError(f"capture/cleanup error: {metadata.get('error') or metadata.get('cleanup_errors')}")
        config = metadata["config"]
        duration = config["duration_s"]
        if type(duration) not in (int, float) or not math.isfinite(duration) or not 0 < duration <= 7200:
            raise QualificationError("invalid requested capture duration")
        baseline = [i for i, row in enumerate(rows) if row.get("phase") == "baseline"]
        final = [i for i, row in enumerate(rows) if row.get("phase") == "final"]
        if len(baseline) != 1 or len(final) != 1 or final[0] <= baseline[0]:
            raise QualificationError("missing or ambiguous steady capture window")
        window = rows[baseline[0]:final[0]+1]
        identity = None
        previous = None
        for row in window:
            data = validate_status(row.get("device"))
            if data.get("source_snapshot_fresh") is not True:
                raise QualificationError("source-counter snapshot was contended/stale")
            if any(data[group][key] == (1 << 64)-1 for group, keys in (("source", SOURCE_KEYS), ("usb", USB_KEYS)) for key in keys):
                raise QualificationError("diagnostic counter saturated; exact deltas unavailable")
            integer(row.get("monotonic_ns"), "host monotonic timestamp")
            host = row["capture"]
            for key in HOST_KEYS:
                integer(host.get(key), f"host.{key}")
            key = tuple(data[name] for name in IDENTITY_KEYS)
            if identity is not None and key != identity:
                raise QualificationError("reset or generation/build/board identity changed within capture")
            identity = key
            if data["serial"] != metadata["serial"] or data["state"] != 3 or not data["generation"]:
                raise QualificationError("capture window is not the same board's RUNNING generation")
            if data["source_rate_millihz"] < 100000 or data["source_rate_millihz"] > 12000000:
                raise QualificationError("invalid measured source rate")
            for name in ("axis", "range_g", "policy", "gain_milli", "cutoff_millihz"):
                if data[name] != config[name]:
                    raise QualificationError("observed configuration differs from requested configuration")
            if config["policy"] < 3 and data["mode"] != config["policy"]:
                raise QualificationError("selected source mode differs from fixed policy")
            if previous is not None:
                old = previous["device"]
                elapsed = data["uptime_us"] - old["uptime_us"]
                if not 0 < elapsed <= MAX_TELEMETRY_GAP_US or row["monotonic_ns"] <= previous["monotonic_ns"]:
                    raise QualificationError("telemetry continuity/timestamp evidence missing")
                if data["source_rate_millihz"] != old["source_rate_millihz"] or data["output_position"] <= old["output_position"]:
                    raise QualificationError("rate changed or USB audio position stalled")
                for group, keys in (("source", SOURCE_KEYS), ("usb", USB_KEYS)):
                    if any(data[group][k] < old[group][k] for k in keys):
                        raise QualificationError(f"{group} counter regression")
                if any(host[k] < previous["capture"][k] for k in HOST_KEYS) or host["frames"] <= previous["capture"]["frames"]:
                    raise QualificationError("host capture stopped or counters regressed")
            previous = row
        first, last = window[0], window[-1]
        a, b = first["device"], last["device"]
        elapsed = {"device_s": (b["uptime_us"]-a["uptime_us"])/1e6,
                   "host_wall_s": (last["monotonic_ns"]-first["monotonic_ns"])/1e9,
                   "host_audio_s": (last["capture"]["frames"]-first["capture"]["frames"])/48000,
                   "usb_audio_s": (b["output_position"]-a["output_position"])/48000}
        result["durations"] = elapsed
        if min(elapsed.values()) < duration:
            raise QualificationError("less than requested continuous capture; command duration is not evidence")
        source = {k: b["source"][k]-a["source"][k] for k in SOURCE_KEYS}
        usb = {k: b["usb"][k]-a["usb"][k] for k in USB_KEYS}
        host = {k: last["capture"][k]-first["capture"][k] for k in HOST_KEYS}
        if not source["intervals"] or source["missed"] > source["intervals"]:
            raise QualificationError("invalid matched-window source interval denominator")
        result.update(source=source, usb=usb, host=host, serial=b["serial"], firmware=b["firmware"],
            elf_sha256=b["elf_sha256"], generation=b["generation"], boot_id=b["boot_id"], qmi=b["qmi"],
            source_rate_millihz=b["source_rate_millihz"], queue_high=b["queue_high"],
            source_miss_fraction=source["missed"]/source["intervals"])
        failures, review = result["failures"], result["review"]
        if any(host[k] for k in ("overflows", "queue_drops", "bad_blocks")):
            failures.append("host capture overflow, missing recording blocks or malformed callbacks")
        if any(usb[k] for k in ("starvation_silence", "failed_silence", "startup_silence", "short_writes")):
            failures.append("steady-state output starvation/silence or short USB FIFO write")
        if source["overruns"] or source["faults"]:
            review.append("source queue overrun/read-window fault")
        if any(usb[k] for k in ("queue_discards", "large_gap_resets", "gap_zeros", "resampler_resets", "clock_saturated", "late_frames",
                                "duplicate_source", "cdc_overflows", "cdc_dropped_replies")):
            review.append("queue/clock/service/CDC observations require engineering disposition")
        if usb["dsp_clips"] or usb["raw_clips"]:
            review.append("source/DSP rail or clipping observations")
        sustained = config["purpose"] == "sustained"
        if sustained:
            if duration < 600 or config["policy"] not in (1, 2) or config["led_state"] != "off":
                raise QualificationError("sustained evidence needs fixed high/medium, 600 seconds and observed LEDs off")
            threshold = 0.001 if config["policy"] == 2 else 0.0001
            result["source_miss_limit"] = threshold
            if source["missed"] / source["intervals"] > threshold:
                failures.append("normative source-miss target exceeded")
            if source["i2c"]:
                (failures if config["policy"] == 1 else review).append("nonzero I2C failures")
        result["result"] = "fail" if failures else "review_required" if review else "pass" if sustained else "evidence_complete"
        result["scope"] = "one USB capture run only; not all CM-009 physical acceptance"
    except (QualificationError, KeyError, TypeError, ValueError) as exc:
        result["failures"].append(str(exc))
        result["result"] = "incomplete"
    return result


def dump(path: Path, data: Any):
    path.write_text(json.dumps(data, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def manifest(directory: Path) -> dict:
    values = {}
    for path in sorted(directory.iterdir()):
        if path.name == "SHA256SUMS.json":
            continue
        if path.is_symlink() or not path.is_file():
            raise QualificationError("evidence must contain only regular files")
        digest = hashlib.sha256()
        with path.open("rb") as f:
            for block in iter(lambda: f.read(65536), b""):
                digest.update(block)
        values[path.name] = digest.hexdigest()
    return values


def summarize(directory: Path) -> dict:
    saved = json.loads((directory / "SHA256SUMS.json").read_text(encoding="utf-8"))
    if manifest(directory) != saved:
        raise QualificationError("evidence hashes changed; raw bundle will not be rewritten")
    rows = []
    with (directory / "telemetry.jsonl").open("rb") as f:
        for _ in range(MAX_ROWS):
            line = f.readline(MAX_ROW_BYTES + 1)
            if not line:
                break
            if len(line) > MAX_ROW_BYTES:
                raise QualificationError("telemetry row exceeds size bound")
            rows.append(json.loads(line))
        if f.read(1):
            raise QualificationError("too many telemetry rows")
    return evaluate(json.loads((directory / "metadata.json").read_text(encoding="utf-8")), rows)


def hardware_modules():
    try:
        import serial
        from serial.tools import list_ports
        import sounddevice
    except (ImportError, OSError) as exc:
        raise QualificationError("Install './host[usb]' and use a working OS USB audio backend") from exc
    return serial, list_ports, sounddevice


def capture(args, *, modules=None) -> int:
    if not math.isfinite(args.duration) or not 0 < args.duration <= 7200:
        raise QualificationError("duration must be >0 and <=7200 seconds")
    if args.purpose == "sustained" and (args.duration < 600 or args.policy not in ("high", "medium") or args.led_state != "off"):
        raise QualificationError("sustained capture requires fixed high/medium, >=600 seconds, observed LED-off baseline")
    if not 0 <= args.gain <= 64 or not math.isfinite(args.gain) or not 0 <= args.high_pass <= 100 or not math.isfinite(args.high_pass):
        raise QualificationError("gain must be 0..64; high-pass cutoff 0..100 Hz")
    serial_api, ports_api, audio_api = modules or hardware_modules()
    ports = list(ports_api.comports()); devices = list(audio_api.query_devices()); apis = list(audio_api.query_hostapis())
    port, audio_index = choose_interfaces(args.serial, ports, devices, apis, args.port, args.audio_device)
    directory = args.output.resolve()
    # mkdir, not rmtree/overwrite. A failed run is retained even when preparation fails.
    directory.mkdir(parents=True, exist_ok=False)
    config = {"axis": "xyz".index(args.axis), "range_g": args.range_g,
        "policy": {"safe": 0, "medium": 1, "high": 2, "auto": 3}[args.policy],
        "gain_milli": round(args.gain * 1000), "cutoff_millihz": round(args.high_pass * 1000),
        "duration_s": args.duration, "purpose": args.purpose, "led_state": args.led_state}
    metadata = {"schema": "cm009.capture.v1", "serial": args.serial, "config": config,
        "created_unix_s": time.time(), "python": sys.version, "platform": platform.platform(),
        "audio_device": dict(devices[audio_index]), "audio_hostapi": dict(apis[devices[audio_index]["hostapi"]]),
        "port": port.device, "note": args.note, "error": None, "cleanup_errors": [],
        "format": {"rate": 48000, "channels": 1, "dtype": "int16"},
        "unavailable": ["physical USB bus error totals", "absolute acceleration calibration"]}
    rows: list[dict] = []
    link = stream = writer = serial_device = None
    buffer = CaptureBuffer(args.wav)
    try:
        serial_device = serial_api.Serial(port=None, baudrate=115200, timeout=0.25, write_timeout=1)
        serial_device.dtr = False; serial_device.rts = False
        serial_device.port = port.device; serial_device.open()
        # This is the matched application CDC interface, not the ROM/JTAG port.
        # TinyUSB connected() requires DTR; application DTR does not drive BOOT.
        serial_device.dtr = True
        link = ControlLink(serial_device, args.serial)
        initial = link.request()
        if initial["state"] != 0:
            raise QualificationError("close all recording applications before configuring the USB microphone")
        metadata["initial_device"] = initial
        link.request("CONFIG", tuple(config[k] for k in ("axis", "range_g", "policy", "gain_milli", "cutoff_millihz")))
        if args.wav:
            writer = WaveWriter(buffer, directory / "capture.wav")
        stream = audio_api.RawInputStream(device=audio_index, samplerate=48000, channels=1,
            dtype="int16", blocksize=480, callback=buffer.callback)
        stream.start()
        start = time.monotonic(); baseline = None
        with (directory / "telemetry.jsonl").open("x", encoding="utf-8") as log:
            while True:
                data = link.request()
                if (data["serial"], data["boot_id"], data["firmware"], data["elf_sha256"]) != tuple(
                        initial[k] for k in ("serial", "boot_id", "firmware", "elf_sha256")):
                    raise QualificationError("board reboot/build/identity changed during capture")
                row = {"monotonic_ns": time.monotonic_ns(), "phase": "startup",
                       "device": data, "capture": asdict(buffer.counts)}
                complete = False
                if baseline is None:
                    if data["state"] == 3 and buffer.counts.frames:
                        row["phase"] = "baseline"; baseline = row
                    elif data["state"] == 4 or time.monotonic()-start > 10:
                        raise QualificationError("source did not reach RUNNING within startup budget")
                else:
                    row["phase"] = "steady"
                    if data["generation"] != baseline["device"]["generation"] or data["state"] != 3:
                        raise QualificationError("capture generation changed or source stopped/starved")
                    elapsed = min((row["monotonic_ns"]-baseline["monotonic_ns"])/1e9,
                        (data["uptime_us"]-baseline["device"]["uptime_us"])/1e6,
                        (data["output_position"]-baseline["device"]["output_position"])/48000,
                        (buffer.counts.frames-baseline["capture"]["frames"])/48000)
                    if elapsed >= args.duration:
                        row["phase"] = "final"; complete = True
                log.write(json.dumps(row, allow_nan=False) + "\n"); log.flush(); rows.append(row)
                if writer and writer.error:
                    raise QualificationError(writer.error)
                if buffer.counts.overflows or buffer.counts.queue_drops or buffer.counts.bad_blocks:
                    raise QualificationError("host input overflow or recording queue loss; run retained")
                if complete:
                    break
                if time.monotonic()-start > args.duration + 20 or len(rows) >= MAX_ROWS - 2:
                    raise QualificationError("capture deadline exceeded without enough valid audio")
                time.sleep(1)
    except KeyboardInterrupt:
        metadata["error"] = "operator aborted capture"
    except Exception as exc:
        metadata["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if stream is not None:
            for method in ("stop", "close"):
                try:
                    getattr(stream, method)()
                except Exception as exc:
                    metadata["cleanup_errors"].append(f"audio {method}: {exc}")
        if writer is not None:
            writer.close()
            if writer.error:
                metadata["cleanup_errors"].append(writer.error)
        if link is not None:
            try:
                metadata["shutdown_device"] = link.request()
            except Exception as exc:
                metadata["cleanup_errors"].append(f"shutdown CDC: {exc}")
        if serial_device is not None:
            try:
                serial_device.close()
            except Exception as exc:
                metadata["cleanup_errors"].append(f"serial close: {exc}")
        metadata["final_capture"] = asdict(buffer.counts)
        dump(directory / "metadata.json", metadata)
        # Early failures still produce a complete, hashable diagnostic bundle.
        if not (directory / "telemetry.jsonl").exists():
            (directory / "telemetry.jsonl").write_text("", encoding="utf-8")
        summary = evaluate(metadata, rows)
        dump(directory / "summary.json", summary)
        dump(directory / "SHA256SUMS.json", manifest(directory))
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["result"] in ("pass", "evidence_complete") else 2


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("devices", help="list native audio inputs and USB serial devices")
    s = sub.add_parser("summarize", help="verify hashes and reevaluate without changing evidence")
    s.add_argument("directory", type=Path)
    p = sub.add_parser("capture", help="record-only USB microphone plus same-board CDC evidence")
    p.add_argument("--serial", required=True)
    p.add_argument("--port")
    p.add_argument("--audio-device", type=int)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--purpose", choices=("preflight", "sustained", "axis-range", "contact"), default="preflight")
    p.add_argument("--duration", type=float, default=10)
    p.add_argument("--axis", choices=("x", "y", "z"), default="z")
    p.add_argument("--range", dest="range_g", type=int, choices=(2, 4), default=2)
    p.add_argument("--policy", choices=("safe", "medium", "high", "auto"), default="safe")
    p.add_argument("--gain", type=float, default=1)
    p.add_argument("--high-pass", type=float, default=20)
    p.add_argument("--led-state", choices=("off", "on", "unknown"), required=True)
    p.add_argument("--wav", action="store_true", help="also save PCM16 audio; long runs create large local files")
    p.add_argument("--note", default="")
    args = parser.parse_args(argv)
    try:
        if args.command == "summarize":
            print(json.dumps(summarize(args.directory), indent=2, sort_keys=True)); return 0
        if args.command == "devices":
            _, ports, audio = hardware_modules()
            print(json.dumps({"audio": list(audio.query_devices()), "hostapis": list(audio.query_hostapis()),
                "serial": [{"port": p.device, "serial": p.serial_number, "vid": p.vid, "pid": p.pid}
                           for p in ports.comports()]}, indent=2)); return 0
        return capture(args)
    except (QualificationError, OSError, ValueError) as exc:
        print(f"cm009: {exc}", file=sys.stderr); return 2


if __name__ == "__main__":
    raise SystemExit(main())
