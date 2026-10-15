from __future__ import annotations

from array import array
from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
import wave

TOOLS = Path(__file__).resolve().parents[2] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from cm007 import common as q
from cm007 import runner as q_runner


def report(*, elapsed, misses=0, i2c=0, ring=0, notify=0, packets=1,
           underrun=0, overrun=0, callbacks=0, backend=0, packet_gaps=0):
    return {
        "schema": q.HOST_RECORD_SCHEMA,
        "elapsed_s": elapsed,
        "report": {
            "firmware": {
                "state": 2,
                "rate_millihz": 3_587_200,
                "mtu": 247,
                "phy": 2,
                "data_length": 251,
                "counters": {
                    "source_misses": misses,
                    "i2c_errors": i2c,
                    "ring_overruns": ring,
                    "notify_rejections": notify,
                    "packets_sent": packets,
                },
            },
            "host": {
                "packet_gaps": packet_gaps,
                "source_gaps": misses,
                "callback_queue_drops": 0,
            },
            "audio": {
                "underrun_samples": underrun,
                "overrun_samples": overrun,
                "callback_failures": callbacks,
                "backend_underflows": backend,
            },
        },
    }


def metadata(policy="medium", purpose="sustained", sink="live", duration=600):
    return {
        "run": {"policy": policy, "purpose": purpose, "sink": sink, "duration_s": duration},
        "environment": {},
        "stimulus": {"error": None},
        "process": {"returncode": 0, "wall_duration_s": duration + 1},
    }


def serial():
    return {
        "port": "COM7", "baud": 115200, "opened": True, "error": None,
        "identity": {"schema": 1, "qmi_address": 107, "who_am_i": 5,
                     "revision": 1, "i2c_hz": 400000, "led_policy": "untouched"},
        "identity_records": 1, "crash_markers": 0, "reset_markers": 1, "lines": 20,
    }


class CM007QualificationTests(unittest.TestCase):
    def test_serial_identity_and_markers(self):
        line = ('I contactmic_hw: CM_HW {"schema":1,"qmi_address":107,'
                '"who_am_i":5,"revision":1,"i2c_hz":400000,'
                '"led_policy":"untouched"}')
        parsed = q.parse_serial_line(line)
        self.assertEqual(parsed["identity"]["qmi_address"], 107)
        self.assertEqual(parsed["crash_markers"], [])
        crash = q.parse_serial_line("Guru Meditation Error: Core  0 panic'ed")
        self.assertEqual(crash["crash_markers"], ["Guru Meditation Error"])

    def test_medium_pass_uses_streaming_counter_deltas(self):
        records = [
            report(elapsed=10, misses=20, packets=100, underrun=48000),
            report(elapsed=610, misses=21, packets=10000, underrun=48000),
        ]
        summary = q.evaluate(records, metadata(), serial())
        self.assertEqual(summary["status"], "pass")
        self.assertEqual(summary["metrics"]["firmware_counter_deltas"]["source_misses"], 1)
        self.assertEqual(summary["metrics"]["audio_counter_deltas"]["underrun_samples"], 0)
        self.assertLess(summary["metrics"]["source_miss_fraction"], 0.0001)

    def test_medium_i2c_and_sustained_underrun_fail(self):
        records = [
            report(elapsed=0, packets=1),
            report(elapsed=600, i2c=1, packets=100, underrun=480),
        ]
        summary = q.evaluate(records, metadata(), serial())
        self.assertEqual(summary["status"], "fail")
        reasons = " ".join(summary["criteria"]["failures_or_incompleteness"])
        self.assertIn("zero I2C errors", reasons)
        self.assertIn("audio underrun", reasons)

    def test_high_i2c_is_warning_not_hidden(self):
        records = [report(elapsed=0), report(elapsed=600, i2c=1, packets=100)]
        summary = q.evaluate(records, metadata(policy="high"), serial())
        self.assertEqual(summary["status"], "pass")
        self.assertIn("prefers zero I2C errors", summary["criteria"]["warnings"][0])

    def test_ring_or_transport_loss_requires_review(self):
        records = [
            report(elapsed=0, packets=1),
            report(elapsed=600, ring=1, notify=1, packets=100, packet_gaps=1),
        ]
        summary = q.evaluate(records, metadata(), serial())
        self.assertEqual(summary["status"], "review_required")
        self.assertEqual(len(summary["criteria"]["review_observations"]), 3)

    def test_missing_identity_is_incomplete(self):
        missing = serial(); missing["identity"] = None; missing["identity_records"] = 0
        summary = q.evaluate([report(elapsed=0), report(elapsed=600)], metadata(), missing)
        self.assertEqual(summary["status"], "incomplete")

    def test_missing_telemetry_fields_are_incomplete(self):
        broken = report(elapsed=600)
        del broken["report"]["firmware"]["counters"]
        records = [report(elapsed=0), broken]
        summary = q.evaluate(records, metadata(), serial())
        self.assertEqual(summary["status"], "incomplete")
        self.assertIn(
            "complete firmware counters",
            " ".join(summary["criteria"]["failures_or_incompleteness"]),
        )

    def test_serial_runtime_error_is_incomplete(self):
        failed = serial(); failed["error"] = "SerialException: device vanished"
        summary = q.evaluate([report(elapsed=0), report(elapsed=600)], metadata(), failed)
        self.assertEqual(summary["status"], "incomplete")

    def test_subwoofer_profile_is_bounded_and_deterministic(self):
        self.assertEqual(q.profile_segment(0, "subwoofer-port-v1", 50), (0, None))
        self.assertEqual(q.profile_segment(5.1, "subwoofer-port-v1", 50), (1, 25.0))
        cycle = sum(duration for duration, _ in q.SUBWOOFER_PROFILE)
        self.assertEqual(q.profile_segment(5.1 + cycle, "subwoofer-port-v1", 50), (1, 25.0))
        self.assertAlmostEqual(q.dbfs_amplitude(-20), 0.1)
        with self.assertRaises(ValueError):
            q.dbfs_amplitude(-6)

    def test_wav_analysis_reports_peak_rms_and_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.wav"
            with wave.open(str(path), "wb") as out:
                out.setnchannels(1); out.setsampwidth(2); out.setframerate(48000)
                out.writeframes(array("h", [0, 16384, -32768, 32767]).tobytes())
            result = q.analyze_wav(path)
            self.assertEqual(result["frames"], 4)
            self.assertEqual(result["clipped_samples"], 2)
            self.assertAlmostEqual(result["peak_fraction"], 1.0)
            self.assertEqual(len(result["sha256"]), 64)

    def test_operational_failure_still_writes_incomplete_evidence(self):
        async def fail_reset(_identifier, _timeout):
            raise RuntimeError("no adapter")
        original = q_runner.reset_diagnostics
        q_runner.reset_diagnostics = fail_reset
        try:
            with tempfile.TemporaryDirectory() as directory:
                args = SimpleNamespace(
                    output=Path(directory) / "run", overwrite=False, purpose="dry-run",
                    device=None, axis="z", range_g=2, policy="safe", duration=1.0,
                    sink="diagnostics", output_device=None, buffer_ms=100, wav=None,
                    raw=False, gain=1.0, high_pass=20.0, conceal_ms=5.0,
                    io_timeout=1.0, serial_port=None, serial_baud=115200,
                    identity_timeout=1.0, allow_incomplete=True, led_state="unknown",
                    fixture_note="", audio_settings_note="", note="", stimulus="none",
                    stimulus_label="", stimulus_output_device=None,
                    stimulus_level_dbfs=-36.0, stimulus_frequency=50.0,
                    stimulus_channels=2, confirm_fixture_safe=False,
                    allow_shared_output=False,
                )
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.assertEqual(q_runner.run_qualification(args), 2)
                summary = __import__("json").loads((args.output / "summary.json").read_text())
                self.assertEqual(summary["status"], "incomplete")
                self.assertTrue((args.output / "SHA256SUMS").exists())
        finally:
            q_runner.reset_diagnostics = original

    def test_official_record_is_bundled_with_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = q_runner.parser().parse_args([
                "run", "--output", str(root / "run"),
                "--purpose", "axis-range", "--duration", "1",
                "--sink", "record", "--wav", str(root / "outside.wav"),
                "--serial-port", "COM7",
            ])
            with self.assertRaises(ValueError):
                q_runner.validate_run_args(args)
            args.wav = args.output / "raw.wav"
            q_runner.validate_run_args(args)

    def test_structured_firmware_identity_contract_is_present(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "firmware" / "main" / "app_main.c").read_text()
        for field in (
            'CM_HW {', 'qmi_address', 'who_am_i', 'revision', 'i2c_hz', 'led_policy'
        ):
            self.assertIn(field, source)

    def test_official_sustained_argument_guards(self):
        args = SimpleNamespace(
            duration=600, purpose="sustained", policy="medium", sink="live",
            output_device="2", wav=None, stimulus="subwoofer-port-v1",
            stimulus_level_dbfs=-36, confirm_fixture_safe=True,
            stimulus_output_device="3", fixture_note="clamped at port mouth",
            audio_settings_note="gain fixed", allow_shared_output=False,
            serial_port="COM7", allow_incomplete=False, led_state="off",
            buffer_ms=100, identity_timeout=20.0, stimulus_frequency=50.0,
        )
        q_runner.validate_run_args(args)
        args.output_device = "3"
        with self.assertRaises(ValueError):
            q_runner.validate_run_args(args)
        args.allow_shared_output = True
        with self.assertRaises(ValueError):
            q_runner.validate_run_args(args)


if __name__ == "__main__":
    unittest.main()
