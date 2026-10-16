"""Regressions against the actual installed capture evaluator, not a fake."""
from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from contactmic import usb_acquisition_audit as a, usb_qualification as u
from test_usb_qualification import evidence


def medium(seconds=600, purpose="sustained"):
    meta, rows = evidence(seconds)
    meta["config"].update(policy=1, purpose=purpose)
    for i, row in enumerate(rows):
        d = row["device"]
        d.update(mode=1, policy=1, source_rate_millihz=3569987)
        d["source"].update(intervals=1000+(i*3569987)//1000, reads=1000+(i*3569987)//1000)
    return meta, rows


def save(directory, meta, rows):
    u.dump(directory/"metadata.json", meta)
    (directory/"telemetry.jsonl").write_text("".join(json.dumps(r)+"\n" for r in rows), encoding="utf-8")
    u.dump(directory/"SHA256SUMS.json", u.manifest(directory))


class IntegrationTests(unittest.TestCase):
    def test_original_false_pass_is_exposed_not_silently_relabelled(self):
        meta, rows = medium()
        for row in rows:
            for key in ("intervals", "reads"): row["device"]["source"][key] //= 2
        # Executable before/after regression: original evaluator accepts this
        # fabricated ledger; the supplementary audit must withhold acceptance.
        self.assertEqual(u.evaluate(meta, rows)["result"], "pass")
        report = a.evaluate(meta, rows)
        self.assertEqual(report["capture_evaluation"]["result"], "pass")
        self.assertEqual(report["result"], "review_required")
        self.assertFalse(report["physical_accepted"])

    def test_success_only_or_unexplained_ledger_cannot_pass(self):
        meta, rows = medium()
        rows[-1]["device"]["source"]["reads"] += 2
        self.assertEqual(u.evaluate(meta, rows)["result"], "pass")
        self.assertEqual(a.evaluate(meta, rows)["result"], "incomplete")

    def test_self_consistent_slow_medium_is_not_accepted(self):
        meta, rows = medium()
        for row in rows:
            row["device"]["source_rate_millihz"] //= 2
            for key in ("intervals", "reads"): row["device"]["source"][key] //= 2
        self.assertEqual(u.evaluate(meta, rows)["result"], "pass")
        self.assertEqual(a.evaluate(meta, rows)["result"], "review_required")

    def test_clean_and_in_flight_windows_keep_actual_denominator(self):
        meta, base = medium()
        for first in (0, 1):
            for last in (0, 1):
                rows = deepcopy(base)
                rows[0]["device"]["source"]["reads"] -= first
                rows[-1]["device"]["source"]["reads"] -= last
                r = a.evaluate(meta, rows)
                self.assertEqual(r["result"], "pass")
                self.assertEqual(r["source_audit"]["window_partition_residual"], last-first)
                self.assertEqual(r["source_audit"]["source_delta"]["intervals"], 2141992)
                self.assertFalse(r["issue_accepted"])

    def test_preflight_already_failing_medium_cannot_progress(self):
        meta, rows = medium(10, "preflight")
        end = rows[-1]["device"]["source"]
        end["reads"] -= 4; end["missed"] = 4
        self.assertEqual(u.evaluate(meta, rows)["result"], "evidence_complete")
        self.assertEqual(a.evaluate(meta, rows)["result"], "fail")
        end["reads"] += 3; end["missed"] = 1; end["i2c"] = 1
        self.assertEqual(a.evaluate(meta, rows)["result"], "fail")

    def test_resets_stale_snapshots_regressions_and_usb_faults_remain_blocking(self):
        meta, base = medium()
        for mutate in (lambda r: r[-1]["device"].update(generation=8),
                       lambda r: r[-1]["device"].update(boot_id=101),
                       lambda r: r[-1]["device"].update(source_snapshot_fresh=False),
                       lambda r: r[-1]["device"]["usb"].update(failed_silence=48),
                       lambda r: r[-1]["device"]["source"].update(intervals=a.UINT64_MAX)):
            rows = deepcopy(base); mutate(rows)
            self.assertIn(a.evaluate(meta, rows)["result"], ("fail", "incomplete"))

    def test_audit_cli_is_fail_closed_and_does_not_rewrite_raw_evidence(self):
        meta, rows = medium(10, "preflight")
        rows[-1]["device"]["source"]["reads"] += 2
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp); save(p, meta, rows)
            before = {x.name: x.read_bytes() for x in p.iterdir()}
            with patch("builtins.print"):
                self.assertEqual(a.main(["audit", str(p)]), 2)
            self.assertEqual(before, {x.name: x.read_bytes() for x in p.iterdir()})
            (p/"metadata.json").write_text("{}", encoding="utf-8")
            with patch("builtins.print"):
                self.assertEqual(a.main(["audit", str(p)]), 2)

    def test_expected_full_build_identities_and_no_device_access(self):
        meta, rows = medium(10, "preflight")
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp); save(p, meta, rows)
            with patch.object(u, "hardware_modules", side_effect=AssertionError("hardware access")), patch("builtins.print"):
                self.assertEqual(a.main(["budget"]), 0)
                self.assertEqual(a.main(["audit", str(p), "--expected-firmware", "a"*40,
                    "--expected-elf-sha256", "b"*64]), 0)
                self.assertEqual(a.main(["audit", str(p), "--expected-firmware", "c"*40]), 2)
                self.assertEqual(a.main(["audit", str(p), "--expected-elf-sha256", "b"*9]), 2)

    def test_oversized_or_additional_artifacts_cannot_bypass_manifest(self):
        meta, rows = medium(10, "preflight")
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp); save(p, meta, rows)
            (p/"new.txt").write_text("unexpected", encoding="utf-8")
            with self.assertRaises(ValueError): a.audit(p)


if __name__ == "__main__":
    unittest.main()
