#!/usr/bin/env python3
"""Run the installed USB qualification module; never add host/src to sys.path."""
from contactmic.usb_qualification import main

if __name__ == "__main__":
    raise SystemExit(main())
