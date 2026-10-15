"""Command-line orchestration for CM-007 hardware evidence."""
from __future__ import annotations

import argparse
import asyncio
from dataclasses import asdict
import json
import math
from pathlib import Path
import platform
import queue
import shutil
import subprocess
import sys
import time
from typing import Any

from .capture import PipeReader, SerialCapture, SerialSummary, StimulusPlayer
from .common import (
    HOST_RECORD_SCHEMA,
    analyze_wav,
    dbfs_amplitude,
    evaluate,
    git_head,
    json_dump,
    load_records,
    utc_now,
    write_manifest,
)


async def reset_diagnostics(identifier: str | None, timeout: float) -> tuple[str, str | None]:
    from contactmic.protocol import CONTROL_UUID, control_request, control_response
    from contactmic.receiver import bleak_api, discover, select_device

    devices = await discover(timeout=min(timeout, 10.0))
    device = select_device(devices, identifier)
    client_factory, _ = bleak_api()
    client = client_factory(device, timeout=timeout)
    try:
        async with asyncio.timeout(timeout):
            await client.connect()
            request_id = 0x7007
            await client.write_gatt_char(
                CONTROL_UUID, control_request(8, request_id), response=True
            )
            response = await client.read_gatt_char(CONTROL_UUID)
            result, payload = control_response(response, 8, request_id)
            if result or payload:
                raise RuntimeError(
                    f"diagnostic reset failed: result={result}, payload={payload.hex()}"
                )
    finally:
        if getattr(client, "is_connected", False):
            await client.disconnect()
    return device.address, getattr(device, "name", None)


def summarize_directory(directory: Path) -> dict[str, Any]:
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    serial_path = directory / "serial-summary.json"
    serial = (
        json.loads(serial_path.read_text(encoding="utf-8"))
        if serial_path.exists() else asdict(SerialSummary(None, 115200))
    )
    summary = evaluate(load_records(directory / "host.jsonl"), metadata, serial)
    json_dump(directory / "summary.json", summary)
    write_manifest(directory)
    return summary


def validate_run_args(args) -> None:
    if not math.isfinite(args.duration) or args.duration <= 0 or args.duration > 7200:
        raise ValueError("duration must be greater than zero and at most 7200 seconds")
    if not 1 <= args.buffer_ms <= 1000:
        raise ValueError("buffer-ms must be 1..1000")
    if not math.isfinite(args.identity_timeout) or args.identity_timeout <= 0:
        raise ValueError("identity-timeout must be positive")
    if not math.isfinite(args.stimulus_frequency) or not 10.0 <= args.stimulus_frequency <= 200.0:
        raise ValueError("stimulus frequency must be between 10 and 200 Hz")
    if args.purpose == "sustained":
        if args.policy not in ("high", "medium"):
            raise ValueError("sustained qualification uses fixed high or medium policy")
        if args.duration < 600:
            raise ValueError("sustained CM-007 qualification requires at least 600 seconds")
        if args.led_state != "off":
            raise ValueError(
                "sustained CM-007 qualification requires an observed LED-off baseline"
            )
        if args.sink != "live":
            raise ValueError("sustained qualification requires --sink live")
    if args.sink == "live" and args.output_device is None:
        raise ValueError("--sink live requires --output-device")
    if args.sink == "record":
        if args.wav is None:
            raise ValueError("--sink record requires --wav")
        if args.purpose != "dry-run" and not args.wav.resolve().is_relative_to(
            args.output.resolve()
        ):
            raise ValueError(
                "official record evidence requires --wav inside the --output directory"
            )
    if args.stimulus != "none":
        dbfs_amplitude(args.stimulus_level_dbfs)
        if not args.confirm_fixture_safe:
            raise ValueError("stimulus requires --confirm-fixture-safe")
        if args.stimulus_output_device is None:
            raise ValueError("stimulus requires --stimulus-output-device")
        if not args.fixture_note or not args.audio_settings_note:
            raise ValueError(
                "stimulus requires --fixture-note and --audio-settings-note"
            )
        same_output = (
            args.sink == "live"
            and str(args.output_device) == str(args.stimulus_output_device)
        )
        if same_output and (args.purpose == "sustained" or not args.allow_shared_output):
            raise ValueError(
                "live sink and stimulus must use different devices; "
                "official sustained evidence cannot override this separation"
            )
    if not args.serial_port and not args.allow_incomplete:
        raise ValueError(
            "official evidence requires --serial-port; use --allow-incomplete only for dry runs"
        )


def build_contactmic_command(args, device: str) -> list[str]:
    command = [sys.executable, "-m", "contactmic"]
    if args.sink == "live":
        command += [
            "monitor", "--output-device", str(args.output_device),
            "--buffer-ms", str(args.buffer_ms),
        ]
    elif args.sink == "record":
        command += ["record", str(args.wav), "--overwrite"]
        if args.raw:
            command.append("--raw")
    else:
        command.append("diagnostics")
    command += [
        "--device", device,
        "--axis", args.axis,
        "--range", str(args.range_g),
        "--policy", args.policy,
        "--duration", str(args.duration),
        "--gain", str(args.gain),
        "--high-pass", str(args.high_pass),
        "--conceal-ms", str(args.conceal_ms),
    ]
    return command


def run_qualification(args) -> int:
    validate_run_args(args)
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()) and not args.overwrite:
        raise FileExistsError(f"evidence directory is not empty: {output}")
    if output.exists() and args.overwrite:
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)

    origin = time.monotonic()
    try:
        import contactmic
        host_version = contactmic.__version__
    except (ImportError, AttributeError):
        host_version = None
    repository_root = Path(__file__).resolve().parents[2]
    metadata: dict[str, Any] = {
        "schema": "contactmic.cm007.metadata.v1",
        "created_utc": utc_now(),
        "run": {
            "purpose": args.purpose,
            "policy": args.policy,
            "axis": args.axis,
            "range_g": args.range_g,
            "duration_s": args.duration,
            "sink": args.sink,
            "led_state_observed": args.led_state,
            "fixture_note": args.fixture_note,
            "operator_note": args.note,
        },
        "environment": {
            "python": sys.version,
            "platform": platform.platform(),
            "repository_head": git_head(repository_root),
            "contactmic_host_version": host_version,
            "argv": sys.argv,
        },
        "stimulus": {
            "profile": args.stimulus,
            "label": args.stimulus_label,
            "level_dbfs": (
                args.stimulus_level_dbfs if args.stimulus != "none" else None
            ),
            "output_device": args.stimulus_output_device,
            "channels": args.stimulus_channels,
            "audio_settings_note": args.audio_settings_note,
            "error": None,
        },
        "process": {},
        "operational_error": None,
    }
    json_dump(output / "metadata.json", metadata)

    serial_capture: SerialCapture | None = None
    stimulus: StimulusPlayer | None = None
    process: subprocess.Popen[str] | None = None
    records: list[dict[str, Any]] = []
    started: float | None = None
    interrupted = False
    operational_error: str | None = None

    try:
        if args.serial_port:
            serial_capture = SerialCapture(
                args.serial_port, args.serial_baud,
                output / "serial.jsonl", origin,
            )
            serial_capture.start()
            opened_or_failed = serial_capture.open_event.wait(5)
            if not opened_or_failed:
                serial_capture.summary.error = "serial open timed out after 5 seconds"
            if (serial_capture.summary.error or not serial_capture.summary.opened) and not args.allow_incomplete:
                raise RuntimeError(serial_capture.summary.error or "serial capture did not open")
            if serial_capture.summary.opened and not serial_capture.summary.error:
                print(
                    f"Serial {args.serial_port} open; press RESET once within "
                    f"{args.identity_timeout:g} seconds to capture CM_HW.",
                    flush=True,
                )
                if serial_capture.identity_event.wait(args.identity_timeout):
                    print("CM_HW identity captured.", flush=True)
                elif not args.allow_incomplete:
                    raise RuntimeError(
                        "CM_HW identity not observed; press RESET once while the runner is waiting"
                    )

        device, device_name = asyncio.run(
            reset_diagnostics(args.device, args.io_timeout)
        )
        metadata["run"]["device"] = device
        metadata["run"]["device_name"] = device_name
        json_dump(output / "metadata.json", metadata)

        if args.stimulus != "none":
            stimulus = StimulusPlayer(
                profile=args.stimulus, duration=args.duration + 30,
                output_device=args.stimulus_output_device,
                level_dbfs=args.stimulus_level_dbfs,
                channels=args.stimulus_channels,
                frequency=args.stimulus_frequency,
                path=output / "stimulus.jsonl", origin=origin,
            )
            stimulus.start()

        command = build_contactmic_command(args, device)
        host_path = output / "host.jsonl"
        stderr_path = output / "host-stderr.log"
        messages: queue.Queue[tuple[str, str | None]] = queue.Queue()
        started = time.monotonic()
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
        )
        assert process.stdout is not None and process.stderr is not None
        PipeReader("stdout", process.stdout, messages).start()
        PipeReader("stderr", process.stderr, messages).start()
        closed: set[str] = set()
        deadline = started + args.duration + args.io_timeout + 60
        with host_path.open(
            "w", encoding="utf-8", newline="\n"
        ) as host_output, stderr_path.open(
            "w", encoding="utf-8", newline="\n"
        ) as error_output:
            while len(closed) < 2 or process.poll() is None:
                if stimulus is not None and stimulus.error and process.poll() is None:
                    raise RuntimeError(f"stimulus output failed: {stimulus.error}")
                if time.monotonic() > deadline and process.poll() is None:
                    raise TimeoutError(
                        "ContactMic host command exceeded its bounded run deadline"
                    )
                try:
                    label, line = messages.get(timeout=0.25)
                except queue.Empty:
                    continue
                if line is None:
                    closed.add(label)
                    continue
                if label == "stderr":
                    error_output.write(line + "\n")
                    error_output.flush()
                    continue
                try:
                    report = json.loads(line)
                except json.JSONDecodeError:
                    error_output.write(f"non-JSON stdout: {line}\n")
                    error_output.flush()
                    continue
                record = {
                    "schema": HOST_RECORD_SCHEMA,
                    "utc": utc_now(),
                    "elapsed_s": round(time.monotonic() - origin, 6),
                    "report": report,
                }
                records.append(record)
                host_output.write(json.dumps(record, sort_keys=True) + "\n")
                host_output.flush()
    except KeyboardInterrupt:
        interrupted = True
        operational_error = "operator interrupted the run"
    except Exception as exc:
        operational_error = f"{type(exc).__name__}: {exc}"
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if stimulus:
            stimulus.stop()
            stimulus.join(timeout=5)
            metadata["stimulus"]["error"] = stimulus.error
        if serial_capture:
            serial_capture.stop()
            serial_capture.join(timeout=5)

    metadata["operational_error"] = operational_error
    metadata["process"] = {
        "command": build_contactmic_command(
            args, metadata["run"].get("device", args.device or "<unresolved>")
        ),
        "returncode": process.returncode if process is not None else None,
        "wall_duration_s": (
            round(time.monotonic() - started, 6) if started is not None else 0.0
        ),
    }
    if args.sink == "record" and args.wav is not None:
        metadata["wav_analysis"] = analyze_wav(args.wav.resolve())
    json_dump(output / "metadata.json", metadata)
    serial_summary = (
        asdict(serial_capture.summary)
        if serial_capture else asdict(SerialSummary(None, args.serial_baud))
    )
    json_dump(output / "serial-summary.json", serial_summary)
    summary = evaluate(records, metadata, serial_summary)
    json_dump(output / "summary.json", summary)
    write_manifest(output)
    print(json.dumps(summary, indent=2, sort_keys=True))
    if interrupted:
        return 130
    if operational_error:
        print(f"cm007-qualify: {operational_error}", file=sys.stderr)
        return 2
    return {
        "pass": 0, "evidence_complete": 0, "review_required": 3,
        "incomplete": 4, "fail": 5,
    }[summary["status"]]


def play_stimulus(args) -> int:
    if not args.confirm_fixture_safe:
        raise ValueError("stimulus requires --confirm-fixture-safe")
    if not math.isfinite(args.duration) or not 0 < args.duration <= 7200:
        raise ValueError("duration must be greater than zero and at most 7200 seconds")
    if (
        not math.isfinite(args.stimulus_frequency)
        or not 10.0 <= args.stimulus_frequency <= 200.0
    ):
        raise ValueError("stimulus frequency must be between 10 and 200 Hz")
    dbfs_amplitude(args.stimulus_level_dbfs)
    output = Path(args.log).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    player = StimulusPlayer(
        profile=args.stimulus, duration=args.duration,
        output_device=args.stimulus_output_device,
        level_dbfs=args.stimulus_level_dbfs,
        channels=args.stimulus_channels,
        frequency=args.stimulus_frequency,
        path=output, origin=time.monotonic(),
    )
    player.start()
    player.join()
    if player.error:
        raise RuntimeError(player.error)
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        description=(
            "Capture CM-007 real-board evidence. A subwoofer-port stimulus is "
            "relative and repeatable, not absolute acceleration calibration."
        )
    )
    commands = root.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="capture one physical qualification run")
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--overwrite", action="store_true")
    run.add_argument(
        "--purpose",
        choices=("sustained", "axis-range", "contact-example", "dry-run"),
        default="dry-run",
    )
    run.add_argument("--device")
    run.add_argument("--axis", choices=("x", "y", "z"), default="z")
    run.add_argument("--range", dest="range_g", type=int, choices=(2, 4), default=2)
    run.add_argument(
        "--policy", choices=("safe", "medium", "high", "auto"), default="safe"
    )
    run.add_argument("--duration", type=float, required=True)
    run.add_argument(
        "--sink", choices=("live", "diagnostics", "record"), default="diagnostics"
    )
    run.add_argument("--output-device")
    run.add_argument("--buffer-ms", type=int, default=100)
    run.add_argument("--wav", type=Path)
    run.add_argument("--raw", action="store_true")
    run.add_argument("--gain", type=float, default=1.0)
    run.add_argument("--high-pass", type=float, default=20.0)
    run.add_argument("--conceal-ms", type=float, default=5.0)
    run.add_argument("--io-timeout", type=float, default=15.0)
    run.add_argument("--serial-port")
    run.add_argument("--serial-baud", type=int, default=115200)
    run.add_argument("--identity-timeout", type=float, default=20.0)
    run.add_argument("--allow-incomplete", action="store_true")
    run.add_argument(
        "--led-state", choices=("off", "on", "unknown"), default="unknown"
    )
    run.add_argument("--fixture-note", default="")
    run.add_argument("--audio-settings-note", default="")
    run.add_argument("--note", default="")
    run.add_argument(
        "--stimulus", choices=("none", "subwoofer-port-v1", "tone"), default="none"
    )
    run.add_argument("--stimulus-label", default="")
    run.add_argument("--stimulus-output-device")
    run.add_argument("--stimulus-level-dbfs", type=float, default=-36.0)
    run.add_argument("--stimulus-frequency", type=float, default=50.0)
    run.add_argument("--stimulus-channels", type=int, choices=(1, 2), default=2)
    run.add_argument("--confirm-fixture-safe", action="store_true")
    run.add_argument("--allow-shared-output", action="store_true")

    summarize = commands.add_parser("summarize", help="recompute summary and hashes")
    summarize.add_argument("directory", type=Path)

    stimulus = commands.add_parser(
        "stimulus", help="play the deterministic fixture stimulus only"
    )
    stimulus.add_argument(
        "--stimulus", choices=("subwoofer-port-v1", "tone"),
        default="subwoofer-port-v1",
    )
    stimulus.add_argument("--duration", type=float, default=90.0)
    stimulus.add_argument("--stimulus-output-device", required=True)
    stimulus.add_argument("--stimulus-level-dbfs", type=float, default=-36.0)
    stimulus.add_argument("--stimulus-frequency", type=float, default=50.0)
    stimulus.add_argument("--stimulus-channels", type=int, choices=(1, 2), default=2)
    stimulus.add_argument("--confirm-fixture-safe", action="store_true")
    stimulus.add_argument("--log", default="build/cm007/stimulus.jsonl")
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "run":
            return run_qualification(args)
        if args.command == "summarize":
            summary = summarize_directory(args.directory.resolve())
            print(json.dumps(summary, indent=2, sort_keys=True))
            return 0
        return play_stimulus(args)
    except KeyboardInterrupt:
        return 130
    except (OSError, RuntimeError, ValueError, TimeoutError) as exc:
        print(f"cm007-qualify: {exc}", file=sys.stderr)
        return 2
