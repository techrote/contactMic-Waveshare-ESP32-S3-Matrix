"""Hardware-free CLI entry point. Receiver commands belong to CM-005."""

import argparse
from collections.abc import Sequence

from . import __version__


def main(argv: Sequence[str] | None = None) -> int:
    """Display foundation help/version; reject commands not implemented yet."""
    parser = argparse.ArgumentParser(
        prog="contactmic",
        description=(
            "ContactMic foundation only: BLE reception and audio processing "
            "are not implemented."
        ),
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    parser.parse_args(argv)
    parser.print_help()
    return 0
