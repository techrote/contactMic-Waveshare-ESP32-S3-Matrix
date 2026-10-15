#!/usr/bin/env python3
"""Entrypoint for the CM-007 physical qualification runner."""
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from cm007.runner import main

if __name__ == "__main__":
    raise SystemExit(main())
