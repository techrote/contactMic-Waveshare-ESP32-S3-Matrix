"""Test CMake guard branches using fixtures, NOT a substitute firmware build."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / "firmware/cmake/toolchain_contract.cmake"


class ToolchainContractTests(unittest.TestCase):
    def check_guard(
        self, version: str | None = "5.5.5", *, target: str | None = None,
        env_target: str | None = None, missing_path: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="contactmic idf ") as temporary:
            root = Path(temporary)
            idf = root / "fake idf"
            if version is not None:
                version_file = idf / "tools/cmake/version.cmake"
                version_file.parent.mkdir(parents=True)
                fields = version.split(".")
                version_file.write_text("".join(
                    f"set(IDF_VERSION_{name} {value})\n"
                    for name, value in zip(("MAJOR", "MINOR", "PATCH"), fields)
                ), encoding="utf-8")
            script = root / "check.cmake"
            script.write_text(
                f'include("{GUARD.as_posix()}")\ncontactmic_check_toolchain()\n',
                encoding="utf-8",
            )
            env = os.environ.copy()
            env.pop("IDF_PATH", None)
            env.pop("IDF_TARGET", None)
            if not missing_path:
                env["IDF_PATH"] = str(idf)
            if env_target is not None:
                env["IDF_TARGET"] = env_target
            command = ["cmake"]
            if target is not None:
                command.append(f"-DIDF_TARGET={target}")
            return subprocess.run(
                [*command, "-P", str(script)], env=env, text=True,
                capture_output=True, check=False, timeout=15,
            )

    def test_exact_version_accepted(self) -> None:
        result = self.check_guard()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_s3_accepted(self) -> None:
        result = self.check_guard(target="esp32s3", env_target="esp32s3")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_other_versions_rejected(self) -> None:
        for version in ("5.5.4", "5.5.6", "5.6.0", "6.0.0", "5.5"):
            with self.subTest(version=version):
                result = self.check_guard(version)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires ESP-IDF v5.5.5", result.stderr)

    def test_missing_environment_rejected(self) -> None:
        result = self.check_guard(missing_path=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("IDF_PATH is not set", result.stderr)

    def test_missing_version_file_rejected(self) -> None:
        result = self.check_guard(None)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("version.cmake", result.stderr)

    def test_wrong_cmake_target_rejected(self) -> None:
        result = self.check_guard(target="esp32")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires IDF_TARGET=esp32s3", result.stderr)

    def test_wrong_environment_target_rejected(self) -> None:
        result = self.check_guard(env_target="esp32c3")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires IDF_TARGET=esp32s3", result.stderr)

    def test_conflicting_target_inputs_rejected(self) -> None:
        for target, env_target in (("esp32s3", "esp32"), ("esp32", "esp32s3")):
            with self.subTest(target=target, env_target=env_target):
                result = self.check_guard(target=target, env_target=env_target)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires IDF_TARGET=esp32s3", result.stderr)


if __name__ == "__main__":
    unittest.main()
