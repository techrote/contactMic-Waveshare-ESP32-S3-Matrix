"""Exercise installed entry points outside the source tree, without hardware."""

import contextlib
import importlib.metadata
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import contactmic
from contactmic.cli import main


class CliTests(unittest.TestCase):
    def run_cli(self, *args: str, console: bool = False, columns: int = 80) -> subprocess.CompletedProcess[str]:
        executable = shutil.which("contactmic") if console else sys.executable
        self.assertIsNotNone(executable, "Install ./host before running tests")
        command = [executable] if console else [executable, "-I", "-m", "contactmic"]
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        env["COLUMNS"] = str(columns)
        with tempfile.TemporaryDirectory() as cwd:
            return subprocess.run(
                [*command, *args], cwd=cwd, env=env, text=True,
                capture_output=True, check=False, timeout=15,
            )

    def test_distribution_metadata(self) -> None:
        dist = importlib.metadata.distribution("contactmic-host")
        self.assertEqual(dist.version, contactmic.__version__)
        self.assertEqual(dist.metadata["Requires-Python"], ">=3.11")
        self.assertFalse(dist.requires)
        self.assertTrue(any(ep.name == "contactmic" for ep in dist.entry_points))
        source = Path(__file__).resolve().parents[1] / "src"
        self.assertFalse(Path(contactmic.__file__).resolve().is_relative_to(source))

    def test_module_help(self) -> None:
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("not implemented", " ".join(result.stdout.split()))
        self.assertEqual(result.stderr, "")

    def test_narrow_terminal_help(self) -> None:
        result = self.run_cli("--help", columns=32)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("not implemented", " ".join(result.stdout.split()))

    def test_console_help(self) -> None:
        result = self.run_cli("--help", console=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("not implemented", " ".join(result.stdout.split()))

    def test_version_both_entry_points(self) -> None:
        for console in (False, True):
            with self.subTest(console=console):
                result = self.run_cli("--version", console=console)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), f"contactmic {contactmic.__version__}")

    def test_no_arguments_are_explicit_help(self) -> None:
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("usage:", result.stdout)
        self.assertIn("not implemented", " ".join(result.stdout.split()))

    def test_unimplemented_commands_are_rejected(self) -> None:
        for args in (("stream",), ("--device", "anything"), ("--not-an-option",)):
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertEqual(result.returncode, 2)
                self.assertIn("unrecognized arguments", result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_callable_entry_point(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(main([]), 0)
        self.assertIn("not implemented", " ".join(output.getvalue().split()))

    def test_import_requires_no_hardware_libraries(self) -> None:
        code = """
import importlib.abc
import sys
class DenyHardwareImports(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname.split('.')[0] in {'bleak', 'sounddevice', 'numpy'}:
            raise AssertionError('unexpected hardware/DSP dependency: ' + fullname)
sys.meta_path.insert(0, DenyHardwareImports())
import contactmic
import contactmic.cli
"""
        with tempfile.TemporaryDirectory() as cwd:
            result = subprocess.run(
                [sys.executable, "-I", "-c", code], cwd=cwd, text=True,
                capture_output=True, check=False, timeout=15,
            )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
