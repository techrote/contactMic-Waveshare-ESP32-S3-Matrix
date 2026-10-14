"""ContactMic CLI. Imports and help do not access Bluetooth or audio devices."""
import argparse
import asyncio
from collections.abc import Sequence
from contextlib import ExitStack
import json
import math
import sys

from . import __version__
from .audio import AudioError, AudioFIFO, LiveSink, WavSink, list_audio_devices
from .dsp import Pipeline
from .protocol import ProtocolError
from .receiver import Receiver, ReceiverConfig, ReceiverError, discover, select_device


def number(text: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a number") from exc
    if not math.isfinite(value):
        raise argparse.ArgumentTypeError("number must be finite")
    return value


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="contactmic", description=(
        "Custom BLE contact-microphone host bridge. Real acquisition integration and "
        "physical qualification are separate firmware milestones."))
    p.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    commands = p.add_subparsers(dest="command")
    scan = commands.add_parser("discover", help="list ContactMic BLE devices")
    scan.add_argument("--timeout", type=number, default=5)
    commands.add_parser("audio-devices", help="list host audio output devices")
    for name, description in (("monitor", "play 48 kHz mono to an output device"),
                              ("record", "record 48 kHz mono PCM16 WAV"),
                              ("diagnostics", "receive and report without an audio device")):
        command = commands.add_parser(name, help=description)
        command.add_argument("--device", help="exact BLE address or unique device name")
        command.add_argument("--axis", choices=("x", "y", "z"), default="z")
        command.add_argument("--range", dest="range_g", type=int, choices=(2, 4), default=2)
        command.add_argument("--policy", choices=("safe", "medium", "high", "auto"), default="safe")
        command.add_argument("--duration", type=number, help="stop after this many seconds; otherwise Ctrl+C")
        command.add_argument("--gain", type=number, default=1)
        command.add_argument("--high-pass", type=number, default=20, metavar="HZ")
        command.add_argument("--conceal-ms", type=number, default=5)
        if name == "monitor":
            command.add_argument("--output-device", help="audio device index or name; use audio-devices")
            command.add_argument("--buffer-ms", type=int, default=100, help="bounded output buffer, 1..1000 ms")
        if name == "record":
            command.add_argument("path", help="output WAV path")
            command.add_argument("--raw", action="store_true", help="pre-DSP samples resampled to 48 kHz; diagnostics retained")
            command.add_argument("--overwrite", action="store_true", help="explicitly permit replacing an existing WAV")
    return p


def report(data: dict) -> None:
    print(json.dumps(data, sort_keys=True), flush=True)


async def run_command(args) -> None:
    if args.command == "discover":
        for device in await discover(args.timeout):
            print(f"{device.address}\t{device.name or '(unnamed)'}")
        return
    if args.command == "audio-devices":
        print(list_audio_devices())
        return
    pipeline = Pipeline(gain=args.gain, cutoff_hz=args.high_pass,
                        conceal_ms=args.conceal_ms, raw=getattr(args, "raw", False))
    config = ReceiverConfig(axis=args.axis, range_g=args.range_g, policy=args.policy, duration=args.duration)
    config.validate()
    fifo = None
    if args.command == "monitor":
        if not 1 <= args.buffer_ms <= 1000:
            raise ValueError("--buffer-ms must be 1..1000")
        fifo = AudioFIFO(args.buffer_ms * 48)
    device = select_device(await discover(), args.device)
    receiver = Receiver(pipeline, config)
    with ExitStack() as stack:
        sink = None
        if args.command == "record":
            sink = stack.enter_context(WavSink(args.path, overwrite=args.overwrite))
        elif args.command == "monitor":
            output = args.output_device
            if output is not None and output.isdecimal():
                output = int(output)
            sink = LiveSink(fifo, output)
            stack.callback(sink.close)
        try:
            await receiver.run(device, sink=sink, report=report)
        finally:
            report(receiver.diagnostics(sink))


def main(argv: Sequence[str] | None = None) -> int:
    p = parser()
    args = p.parse_args(argv)
    if args.command is None:
        p.print_help()
        return 0
    try:
        asyncio.run(run_command(args))
    except KeyboardInterrupt:
        return 130
    except (ReceiverError, AudioError, ProtocolError, ValueError, OSError) as exc:
        print(f"contactmic: {exc}", file=sys.stderr)
        return 2
    return 0
