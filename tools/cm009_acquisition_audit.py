#!/usr/bin/env python3
"""Use the installed offline acquisition auditor, never a source-path override."""
from contactmic.usb_acquisition_audit import main

if __name__ == "__main__":
    raise SystemExit(main())
