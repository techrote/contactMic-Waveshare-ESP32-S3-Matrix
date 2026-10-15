"""Pure CM-007 evidence parsing, stimulus description and qualification rules."""
from __future__ import annotations

from array import array
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import wave
from typing import Any, Iterable

SCHEMA = "contactmic.cm007.summary.v1"
HOST_RECORD_SCHEMA = "contactmic.cm007.host-record.v1"
SERIAL_RECORD_SCHEMA = "contactmic.cm007.serial-record.v1"
STIMULUS_RECORD_SCHEMA = "contactmic.cm007.stimulus-record.v1"

CM_HW_RE = re.compile(r"CM_HW\s+(\{.*\})")
CRASH_MARKERS = (
    "Guru Meditation Error",
    "assert failed:",
    "abort() was called",
    "Stack canary watchpoint triggered",
    "Brownout detector was triggered",
)
RESET_MARKERS = ("ESP-ROM:", "rst:")
COUNTER_NAMES = (
    "source_misses",
    "i2c_errors",
    "ring_overruns",
    "notify_rejections",
    "packets_sent",
)
POLICY_MISS_LIMITS = {"high": 0.001, "medium": 0.0001}

SUBWOOFER_PROFILE: tuple[tuple[float, float | None], ...] = (
    (5.0, None),
    (8.0, 25.0), (2.0, None),
    (8.0, 31.5), (2.0, None),
    (8.0, 40.0), (2.0, None),
    (8.0, 50.0), (2.0, None),
    (8.0, 63.0), (2.0, None),
    (8.0, 80.0), (3.0, None),
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def json_dump(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_device(value: str | None):
    if value is None:
        return None
    return int(value) if value.isdecimal() else value


def dbfs_amplitude(value: float) -> float:
    if not math.isfinite(value) or not -60.0 <= value <= -12.0:
        raise ValueError("stimulus level must be between -60 and -12 dBFS")
    return 10.0 ** (value / 20.0)


def git_head(cwd: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=cwd, text=True,
            capture_output=True, check=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    value = result.stdout.strip()
    return value if re.fullmatch(r"[0-9a-fA-F]{40}", value) else None


def parse_serial_line(line: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "identity": None,
        "crash_markers": [marker for marker in CRASH_MARKERS if marker in line],
        "reset_markers": [marker for marker in RESET_MARKERS if marker in line],
    }
    match = CM_HW_RE.search(line)
    if match:
        try:
            identity = json.loads(match.group(1))
        except json.JSONDecodeError:
            result["identity_error"] = "malformed CM_HW JSON"
        else:
            if isinstance(identity, dict) and identity.get("schema") == 1:
                result["identity"] = identity
            else:
                result["identity_error"] = "unsupported CM_HW schema"
    return result


def profile_segment_detail(
    elapsed: float, profile: str, frequency: float, total_duration: float | None = None
) -> tuple[int, float | None, float, float]:
    if profile == "tone":
        duration = total_duration if total_duration is not None else float("inf")
        return 0, frequency, elapsed, duration
    if profile != "subwoofer-port-v1":
        return 0, None, elapsed, total_duration or float("inf")
    cycle = sum(duration for duration, _ in SUBWOOFER_PROFILE)
    position = elapsed % cycle
    for index, (duration, hz) in enumerate(SUBWOOFER_PROFILE):
        if position < duration:
            return index, hz, position, duration
        position -= duration
    duration, hz = SUBWOOFER_PROFILE[-1]
    return len(SUBWOOFER_PROFILE) - 1, hz, duration, duration


def profile_segment(elapsed: float, profile: str, frequency: float) -> tuple[int, float | None]:
    index, hz, _, _ = profile_segment_detail(elapsed, profile, frequency)
    return index, hz


def analyze_wav(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"error": "WAV file was not created"}
    try:
        with wave.open(str(path), "rb") as source:
            channels = source.getnchannels()
            width = source.getsampwidth()
            rate = source.getframerate()
            frames = source.getnframes()
            if width != 2:
                return {"error": f"unsupported sample width: {width}"}
            raw = source.readframes(frames)
    except (OSError, wave.Error) as exc:
        return {"error": f"{type(exc).__name__}: {exc}"}
    samples = array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        return {
            "channels": channels, "sample_rate": rate, "frames": frames,
            "duration_s": 0.0, "peak_fraction": 0.0, "rms_fraction": 0.0,
            "clipped_samples": 0,
        }
    peak = max(abs(int(value)) for value in samples)
    square_mean = sum(float(value) * float(value) for value in samples) / len(samples)
    clipped = sum(abs(int(value)) >= 32767 for value in samples)
    return {
        "channels": channels,
        "sample_rate": rate,
        "frames": frames,
        "duration_s": frames / rate if rate else 0.0,
        "peak_fraction": peak / 32768.0,
        "rms_fraction": math.sqrt(square_mean) / 32768.0,
        "clipped_samples": clipped,
        "sha256": sha256_file(path),
    }


def nested(report: dict[str, Any], *keys: str, default: Any = None) -> Any:
    value: Any = report
    for key in keys:
        if not isinstance(value, dict) or key not in value:
            return default
        value = value[key]
    return value


def difference(first: dict[str, Any], last: dict[str, Any], path: Iterable[str]) -> int:
    keys = tuple(path)
    a = nested(first, *keys, default=0)
    b = nested(last, *keys, default=0)
    if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
        return 0
    return max(0, int(b) - int(a))


def evaluate(
    records: list[dict[str, Any]], metadata: dict[str, Any], serial: dict[str, Any]
) -> dict[str, Any]:
    reports = [record for record in records if isinstance(record.get("report"), dict)]
    streaming = [record for record in reports if nested(record["report"], "firmware", "state") == 2]
    reasons: list[str] = []
    warnings: list[str] = []
    observations: list[str] = []
    metrics: dict[str, Any] = {}

    if len(streaming) < 2:
        reasons.append("fewer than two streaming telemetry snapshots")
    else:
        first, last = streaming[0], streaming[-1]
        first_report, last_report = first["report"], last["report"]
        window = max(0.0, float(last["elapsed_s"]) - float(first["elapsed_s"]))
        rate_millihz = int(nested(last_report, "firmware", "rate_millihz", default=0) or 0)
        first_counters = nested(first_report, "firmware", "counters")
        last_counters = nested(last_report, "firmware", "counters")
        counters_complete = all(
            isinstance(source, dict)
            and all(isinstance(source.get(name), (int, float)) for name in COUNTER_NAMES)
            for source in (first_counters, last_counters)
        )
        if not counters_complete:
            reasons.append("streaming telemetry lacks complete firmware counters")
        fw_deltas = {
            name: difference(first_report, last_report, ("firmware", "counters", name))
            for name in COUNTER_NAMES
        }
        audio_names = (
            "underrun_samples", "overrun_samples", "callback_failures", "backend_underflows"
        )
        first_audio = nested(first_report, "audio")
        last_audio = nested(last_report, "audio")
        audio_complete = all(
            isinstance(source, dict)
            and all(isinstance(source.get(name), (int, float)) for name in audio_names)
            for source in (first_audio, last_audio)
        )
        audio_deltas = {
            name: difference(first_report, last_report, ("audio", name))
            for name in audio_names
        }
        expected_intervals = rate_millihz * window / 1000.0
        miss_fraction = (
            fw_deltas["source_misses"] / expected_intervals if expected_intervals > 0 else None
        )
        metrics.update({
            "qualified_window_s": round(window, 6),
            "measured_rate_millihz": rate_millihz,
            "expected_source_intervals": round(expected_intervals, 3),
            "firmware_counter_start": first_counters,
            "firmware_counter_end": last_counters,
            "firmware_counter_deltas": fw_deltas,
            "firmware_final": nested(last_report, "firmware", default={}),
            "host_final": nested(last_report, "host", default={}),
            "audio_counter_start": first_audio,
            "audio_counter_end": last_audio,
            "audio_counter_deltas": audio_deltas,
            "mtu": nested(last_report, "firmware", "mtu"),
            "phy": nested(last_report, "firmware", "phy"),
            "data_length": nested(last_report, "firmware", "data_length"),
            "source_miss_fraction": miss_fraction,
            "streaming_snapshots": len(streaming),
        })

        requested = float(metadata["run"]["duration_s"])
        if metadata["run"]["purpose"] == "sustained":
            if metadata.get("process", {}).get("wall_duration_s", 0) < requested - 0.5:
                reasons.append("sustained run ended before the requested duration")
            if window < requested - 2.5:
                reasons.append("streaming telemetry window is shorter than the requested duration")
            if rate_millihz <= 0:
                reasons.append("streaming telemetry lacks a positive measured source rate")
            if fw_deltas["packets_sent"] <= 0:
                reasons.append("no firmware packets were sent during the qualified window")
            if metadata["run"]["sink"] == "live" and not audio_complete:
                reasons.append("streaming telemetry lacks complete live-audio counters")
        policy = metadata["run"]["policy"]
        if metadata["run"]["purpose"] == "sustained":
            limit = POLICY_MISS_LIMITS.get(policy)
            if limit is None:
                reasons.append("sustained acceptance requires fixed high or medium policy")
            elif miss_fraction is None or miss_fraction > limit:
                reasons.append(f"source miss fraction exceeds {limit:.4%} {policy} target")
            if policy == "medium" and fw_deltas["i2c_errors"] != 0:
                reasons.append("medium qualification requires zero I2C errors")
            elif policy == "high" and fw_deltas["i2c_errors"] != 0:
                warnings.append("high target prefers zero I2C errors")
            if metadata["run"]["sink"] != "live":
                reasons.append("sustained qualification requires the live host audio sink")
            elif any(audio_deltas[name] for name in ("underrun_samples", "callback_failures", "backend_underflows")):
                reasons.append("host audio underrun/callback failure occurred after startup baseline")

        if fw_deltas["ring_overruns"]:
            observations.append("ring overruns require engineering review")
        if fw_deltas["notify_rejections"]:
            observations.append("BLE notify rejections require engineering review")
        host_final = metrics["host_final"]
        if isinstance(host_final, dict) and host_final.get("packet_gaps", 0):
            observations.append("host packet gaps require transport review")
        if isinstance(host_final, dict) and host_final.get("callback_queue_drops", 0):
            observations.append("host callback queue drops require review")
        if audio_deltas.get("overrun_samples", 0):
            observations.append("host audio FIFO overrun requires review")

    if not serial.get("opened"):
        reasons.append("serial capture did not open")
    if serial.get("error"):
        reasons.append(f"serial capture failed: {serial['error']}")
    identity = serial.get("identity")
    if identity is None:
        reasons.append("CM_HW board identity was not captured")
    else:
        required_identity = {
            "schema", "qmi_address", "who_am_i", "revision", "i2c_hz", "led_policy"
        }
        missing_identity = sorted(required_identity - set(identity))
        if missing_identity:
            reasons.append(
                "CM_HW identity missing required fields: " + ", ".join(missing_identity)
            )
    if serial.get("identity_records", 0) > 1:
        reasons.append("more than one firmware boot identity was observed during the run")
    if serial.get("crash_markers", 0):
        reasons.append("firmware crash marker observed")
    if metadata.get("stimulus", {}).get("error"):
        reasons.append("stimulus output failed")
    if metadata.get("operational_error"):
        reasons.append("qualification runner did not complete normally")
    if metadata.get("process", {}).get("returncode") not in (0, None):
        reasons.append("ContactMic host command failed")

    purpose = metadata["run"]["purpose"]
    if reasons:
        incomplete_prefixes = (
            "serial capture", "CM_HW", "fewer than", "sustained acceptance",
            "sustained run ended", "streaming telemetry",
        )
        status = "incomplete" if any(
            text.startswith(incomplete_prefixes) for text in reasons
        ) else "fail"
    elif observations:
        status = "review_required"
    else:
        status = "pass" if purpose == "sustained" else "evidence_complete"

    return {
        "schema": SCHEMA,
        "generated_utc": utc_now(),
        "status": status,
        "run": metadata["run"],
        "environment": metadata.get("environment", {}),
        "stimulus": metadata.get("stimulus", {}),
        "serial": serial,
        "wav_analysis": metadata.get("wav_analysis"),
        "metrics": metrics,
        "criteria": {
            "failures_or_incompleteness": reasons,
            "warnings": warnings,
            "review_observations": observations,
            "normative_source_miss_limit": POLICY_MISS_LIMITS.get(metadata["run"]["policy"]),
        },
    }


def write_manifest(directory: Path) -> None:
    entries = []
    for path in sorted(directory.iterdir(), key=lambda item: item.name):
        if path.is_file() and path.name != "SHA256SUMS":
            entries.append(f"{sha256_file(path)}  {path.name}")
    (directory / "SHA256SUMS").write_text("\n".join(entries) + "\n", encoding="utf-8")


def load_records(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    if not path.exists():
        return records
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{path}:{number}: invalid JSON: {exc}") from exc
        if isinstance(value, dict):
            records.append(value)
    return records
