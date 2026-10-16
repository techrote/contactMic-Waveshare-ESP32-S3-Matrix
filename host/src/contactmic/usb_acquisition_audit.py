"""Offline CM-009 acquisition review; never opens a device or changes evidence.

This supplements, rather than replaces, usb_qualification. Its rate tripwire is
NOT a vendor tolerance or permission to lose samples. A clean audit is only
internal consistency: it cannot prove free-running conversions or provenance.
"""
from __future__ import annotations

import argparse
from fractions import Fraction
import json
from pathlib import Path
import re
import sys
from typing import Any

UINT64_MAX = (1 << 64) - 1
COUNTERS = ("intervals", "reads", "missed", "i2c", "overruns", "faults")
# Reference only. Never used to construct an interval denominator.
NOMINAL_MILLIHZ = (896800, 3587200, 7174400)
# A gross-discrepancy review trigger, matching the existing USB servo range.
# It is NOT the 0.01% MEDIUM loss limit or a guaranteed sensor ODR tolerance.
RATE_REVIEW_FRACTION = Fraction(3, 100)


def _uint(value: Any, name: str, maximum: int = UINT64_MAX) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"invalid unsigned {name}")
    return value


def register_read_bits(payload_bytes: int) -> int:
    """Address-W, register, address-R, payload; each byte has ACK/NACK clock."""
    if type(payload_bytes) is not int or not 1 <= payload_bytes <= 1536:
        raise ValueError("payload must be 1..1536 bytes")
    return 9 * (3 + payload_bytes)


def budget(source_rate_millihz: int = 3569987, i2c_hz: int = 400000) -> dict:
    """Optimistic wire floors, NOT measured timing or an implemented driver."""
    rate = _uint(source_rate_millihz, "source rate", 12000000)
    bus = _uint(i2c_hz, "I2C clock", 400000)
    if not rate or not bus:
        raise ValueError("source rate and I2C clock must be positive")
    status = register_read_bits(1)
    transactions = {
        "selected_axis": register_read_bits(2),
        # This deliberately grants sparse reads not established by the vendor
        # burst procedure. Even that optimistic shortcut cannot save MEDIUM.
        "optimistic_sparse_lock": status + register_read_bits(2) + register_read_bits(1),
        "documented_z_through_gz_h": status + register_read_bits(8),
        "documented_y_through_gz_h": status + register_read_bits(10),
        "documented_x_through_gz_h": status + register_read_bits(12),
        "fifo_one_6dof_record": register_read_bits(12),
        "fifo_128_6dof_records": register_read_bits(128 * 12),
    }
    detail = {}
    for name, bits in transactions.items():
        wire_us = Fraction(bits * 1000000, bus)
        detail[name] = {"scl_periods": bits, "wire_us_minimum": float(wire_us)}
        if name in ("optimistic_sparse_lock", "documented_z_through_gz_h",
                    "documented_y_through_gz_h", "documented_x_through_gz_h"):
            capacity = Fraction(bus, bits)
            detail[name].update(max_reads_per_s=float(capacity),
                minimum_unserved_fraction=float(max(Fraction(0), 1 - capacity * 1000 / rate)))
    return {"schema": "cm009.acquisition-budget.v1", "physical_accepted": False,
        "reference_source_rate_millihz": rate, "i2c_hz": bus,
        "source_period_us": float(Fraction(1000000000, rate)), "transactions": detail,
        "fifo_payload_bus_fraction": float(Fraction(12 * 9 * rate, 1000 * bus)),
        "excluded": ["START/STOP/setup and bus-free time", "clock stretching", "lock delay",
            "CTRL9 setup/handshake and FIFO-mode exit", "driver/ISR/RTOS and scheduling"],
        "limits": ["QST QMI8658C 13-52-27 Rev A sections 8, 13 and 16; see repository vendor review",
            "Sparse locking is a hypothetical optimistic bound, not an authorized sequence",
            "FIFO discards new data during read mode; payload utilization is not feasibility",
            "Default cadence is a historical reported reference, not a live measurement"]}


def ledger(window: list[dict]) -> dict:
    """Check a previously selected baseline-to-final window; no acceptance claim.

    The caller must ALSO apply usb_qualification.evaluate for identity, config,
    telemetry continuity, all four duration clocks, USB and host integrity.
    """
    if not isinstance(window, list) or len(window) < 2:
        raise ValueError("source audit requires a matched window")
    pending = []
    previous = None
    for row in window:
        device = row["device"]
        if device.get("source_snapshot_fresh") is not True:
            raise ValueError("stale source snapshot")
        source = device["source"]
        values = {k: _uint(source[k], k) for k in COUNTERS}
        if any(v == UINT64_MAX for v in values.values()):
            raise ValueError("source counter saturated; exact accounting unavailable")
        # The real producer accounts the interval, completes its synchronous
        # read outside the counter seqlock, then accounts read success/failure.
        work = values["intervals"] - values["reads"] - values["missed"]
        if work not in (0, 1):
            raise ValueError("source ledger is not intervals = reads + missed + one-or-zero in-flight")
        if values["i2c"] + values["faults"] > values["missed"]:
            raise ValueError("read failures exceed accounted source misses")
        if values["overruns"] > values["reads"]:
            raise ValueError("ring discards exceed successful reads")
        if previous is not None:
            if any(values[k] < previous[k] for k in COUNTERS):
                raise ValueError("source counter regression")
            if values["intervals"] <= previous["intervals"]:
                raise ValueError("source interval progress stalled")
        previous = values
        pending.append(work)
    first, last = window[0], window[-1]
    a, b = first["device"], last["device"]
    elapsed_us = _uint(b["uptime_us"], "uptime") - _uint(a["uptime_us"], "uptime")
    host_ns = _uint(last["monotonic_ns"], "host time") - _uint(first["monotonic_ns"], "host time")
    if elapsed_us <= 0 or host_ns <= 0:
        raise ValueError("non-positive source-audit elapsed time")
    delta = {k: b["source"][k] - a["source"][k] for k in COUNTERS}
    if not delta["intervals"] or delta["missed"] > delta["intervals"]:
        raise ValueError("invalid actual source interval denominator")
    measured = _uint(b["source_rate_millihz"], "preparation rate", 12000000)
    mode = _uint(b["mode"], "source mode", 2)
    if not measured:
        raise ValueError("zero preparation rate")
    interval_millihz = Fraction(delta["intervals"] * 1000000000, elapsed_us)
    discrepancy = interval_millihz / measured - 1
    nominal_discrepancy = Fraction(measured, NOMINAL_MILLIHZ[mode]) - 1
    review = []
    if abs(discrepancy) > RATE_REVIEW_FRACTION:
        review.append("actual interval cadence differs from preparation rate by more than the 3% review tripwire")
    if abs(nominal_discrepancy) > RATE_REVIEW_FRACTION:
        review.append("preparation rate differs from commanded mode's nominal reference by more than the 3% review tripwire")
    return {"source_delta": delta, "in_flight_first": pending[0], "in_flight_last": pending[-1],
        "window_partition_residual": delta["intervals"] - delta["reads"] - delta["missed"],
        "interval_hz_device": float(interval_millihz / 1000),
        "interval_hz_host": float(Fraction(delta["intervals"] * 1000000000, host_ns)),
        "successful_read_hz_device": float(Fraction(delta["reads"] * 1000000, elapsed_us)),
        "source_miss_fraction": delta["missed"] / delta["intervals"],
        "interval_vs_preparation_fraction": float(discrepancy),
        "preparation_vs_nominal_reference_fraction": float(nominal_discrepancy),
        "review": review,
        "limits": "3% is a review tripwire, not an ODR guarantee or loss allowance; internal consistency does not prove an unpaused source"}


def evaluate(metadata: dict, rows: list[dict]) -> dict:
    from . import usb_qualification as capture
    base = capture.evaluate(metadata, rows)
    result = {"schema": "cm009.acquisition-audit.v1", "result": base["result"],
        "issue_accepted": False, "physical_accepted": False, "capture_evaluation": base,
        "failures": list(base["failures"]), "review": list(base["review"])}
    if base["result"] == "incomplete":
        return result
    try:
        start = next(i for i, r in enumerate(rows) if r.get("phase") == "baseline")
        end = next(i for i, r in enumerate(rows) if r.get("phase") == "final")
        result["source_audit"] = report = ledger(rows[start:end + 1])
        result["review"].extend(report["review"])
        # Preflight must not be called credible while already failing the fixed
        # MEDIUM source target. Actual intervals remain the denominator.
        if metadata["config"]["policy"] == 1:
            if report["source_delta"]["missed"] * 10000 > report["source_delta"]["intervals"]:
                result["failures"].append("fixed MEDIUM source-miss target exceeded")
            if report["source_delta"]["i2c"]:
                result["failures"].append("fixed MEDIUM has nonzero I2C failures")
        if result["failures"]:
            result["result"] = "fail"
        elif result["review"]:
            result["result"] = "review_required"
    except (KeyError, TypeError, ValueError, StopIteration) as exc:
        result["result"] = "incomplete"
        result["failures"].append(str(exc))
    return result


def audit(directory: Path) -> dict:
    """Read a bounded, hash-verified bundle without adding/changing any file."""
    from . import usb_qualification as capture
    saved = json.loads((directory / "SHA256SUMS.json").read_text(encoding="utf-8"))
    if capture.manifest(directory) != saved:
        raise ValueError("evidence hashes changed")
    metadata_path = directory / "metadata.json"
    if metadata_path.stat().st_size > 1048576:
        raise ValueError("metadata exceeds size bound")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    rows = []
    with (directory / "telemetry.jsonl").open("rb") as handle:
        for _ in range(capture.MAX_ROWS):
            raw = handle.readline(capture.MAX_ROW_BYTES + 1)
            if not raw:
                break
            if len(raw) > capture.MAX_ROW_BYTES:
                raise ValueError("telemetry row exceeds size bound")
            rows.append(json.loads(raw))
        if handle.read(1):
            raise ValueError("too many telemetry rows")
    result = evaluate(metadata, rows)
    if capture.manifest(directory) != saved:
        raise ValueError("evidence changed during audit")
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("budget", help="print optimistic wire floors, not hardware timing")
    p.add_argument("--source-rate-millihz", type=int, default=3569987)
    p.add_argument("--i2c-hz", type=int, default=400000)
    p = commands.add_parser("audit", help="supplement the existing immutable capture-bundle review")
    p.add_argument("directory", type=Path)
    p.add_argument("--expected-firmware")
    p.add_argument("--expected-elf-sha256")
    args = parser.parse_args(argv)
    try:
        if args.command == "budget":
            print(json.dumps(budget(args.source_rate_millihz, args.i2c_hz), indent=2, allow_nan=False))
            return 0
        for value, length, name in ((args.expected_firmware, 40, "firmware"),
                                    (args.expected_elf_sha256, 64, "ELF")):
            if value is not None and not re.fullmatch(r"[0-9a-fA-F]{" + str(length) + "}", value):
                raise ValueError(f"expected {name} must be a complete hexadecimal identity")
        report = audit(args.directory)
        base = report["capture_evaluation"]
        for expected, name in ((args.expected_firmware, "firmware"),
                                (args.expected_elf_sha256, "elf_sha256")):
            if expected and str(base.get(name, "")).lower() != expected.lower():
                report["result"] = "incomplete"
                report["failures"].append(f"operator build {name} does not match capture identity")
        print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
        return 0 if report["result"] in ("pass", "evidence_complete") else 2
    except (OSError, ValueError, TypeError, KeyError, RuntimeError) as exc:
        print(f"cm009 acquisition audit: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
