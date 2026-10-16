#!/usr/bin/env python3
"""Verify a non-editable, isolated host install against the checked-out sources."""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    if sys.prefix == sys.base_prefix:
        raise RuntimeError("host verification requires an isolated virtual environment")
    import contactmic
    source = (args.source / "src" / "contactmic").resolve(strict=True)
    installed = Path(contactmic.__file__).resolve(strict=True).parent
    prefix = Path(sys.prefix).resolve(strict=True)
    if not installed.is_relative_to(prefix) or installed.is_relative_to(source):
        raise RuntimeError("contactmic was not imported from this virtual environment")
    dist = importlib.metadata.distribution("contactmic-host")
    provenance = json.loads(dist.read_text("direct_url.json") or "null")
    if not isinstance(provenance, dict) or provenance.get("dir_info", {}).get("editable"):
        raise RuntimeError("missing or editable installation provenance")
    expected = {p.relative_to(source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in source.rglob("*.py")}
    observed = {p.relative_to(installed).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in installed.rglob("*.py")}
    if not expected or expected != observed:
        raise RuntimeError("installed Python sources differ from the checked-out package")
    if dist.version != contactmic.__version__:
        raise RuntimeError("distribution and package versions differ")
    print(json.dumps({"python": sys.version, "executable": sys.executable,
        "prefix": str(prefix), "package": str(installed), "version": dist.version,
        "direct_url": provenance, "source_sha256": expected}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
