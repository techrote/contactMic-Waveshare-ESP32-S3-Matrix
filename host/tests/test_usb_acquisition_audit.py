"""Synthetic arithmetic/ledger tests; no physical sensor or timing claims."""
from copy import deepcopy
from fractions import Fraction
import unittest

from contactmic import usb_acquisition_audit as a


def window():
    def row(t, intervals):
        return {"monotonic_ns": (t + 100) * 1000000000,
            "device": {"uptime_us": (t + 100) * 1000000, "source_snapshot_fresh": True,
                "source_rate_millihz": 3587200, "mode": 1,
                "source": dict(intervals=intervals, reads=intervals, missed=0, i2c=0, overruns=0, faults=0)}}
    return [row(0, 1000), row(10, 36872), row(20, 72744)]


class BudgetTests(unittest.TestCase):
    def test_wire_floors_and_medium_capacity(self):
        b = a.budget()
        self.assertFalse(b["physical_accepted"])
        t = b["transactions"]
        self.assertEqual(a.register_read_bits(2), 45)
        self.assertEqual(t["selected_axis"]["wire_us_minimum"], 112.5)
        self.assertEqual(t["optimistic_sparse_lock"]["scl_periods"], 117)
        self.assertEqual(t["optimistic_sparse_lock"]["wire_us_minimum"], 292.5)
        self.assertGreater(292.5, b["source_period_us"])
        self.assertGreater(t["optimistic_sparse_lock"]["minimum_unserved_fraction"], .04)
        self.assertEqual(t["documented_z_through_gz_h"]["wire_us_minimum"], 337.5)
        self.assertEqual(t["documented_y_through_gz_h"]["wire_us_minimum"], 382.5)
        self.assertEqual(t["documented_x_through_gz_h"]["wire_us_minimum"], 427.5)
        self.assertEqual(t["fifo_128_6dof_records"]["wire_us_minimum"], 34627.5)
        self.assertAlmostEqual(b["fifo_payload_bus_fraction"], .96389649)

    def test_slower_bus_cannot_repair_the_bound(self):
        for bus in (100000, 200000, 400000):
            b = a.budget(i2c_hz=bus)
            self.assertGreater(b["transactions"]["optimistic_sparse_lock"]["wire_us_minimum"], b["source_period_us"])
        # This says only that a wire floor fits SAFE, not that SAFE passes.
        safe = a.budget(896800)
        self.assertEqual(safe["transactions"]["optimistic_sparse_lock"]["minimum_unserved_fraction"], 0)
        self.assertFalse(safe["physical_accepted"])

    def test_invalid_and_overclock_inputs(self):
        for value in (0, -1, True, 400001, 1000000):
            with self.assertRaises(ValueError): a.budget(i2c_hz=value)
        for value in (0, -1, True, 12000001):
            with self.assertRaises(ValueError): a.budget(value)
        for value in (0, -1, True, 1537):
            with self.assertRaises(ValueError): a.register_read_bits(value)


class LedgerTests(unittest.TestCase):
    def test_actual_rates_not_nominal_denominator(self):
        r = a.ledger(window())
        self.assertEqual(r["source_delta"]["intervals"], 71744)
        self.assertEqual(r["source_miss_fraction"], 0)
        self.assertEqual(r["interval_hz_device"], 3587.2)
        self.assertEqual(r["interval_hz_host"], 3587.2)
        self.assertEqual(r["successful_read_hz_device"], 3587.2)
        self.assertEqual(r["review"], [])

    def test_one_in_flight_and_matched_boundary_residuals(self):
        for first in (0, 1):
            for last in (0, 1):
                rows = window()
                rows[0]["device"]["source"]["reads"] -= first
                rows[-1]["device"]["source"]["reads"] -= last
                r = a.ledger(rows)
                self.assertEqual((r["in_flight_first"], r["in_flight_last"]), (first, last))
                self.assertEqual(r["window_partition_residual"], last-first)

    def test_unexplained_counts_and_success_only_denominator(self):
        for field, difference in (("reads", 2), ("reads", -2), ("missed", 2), ("i2c", 1), ("faults", 1)):
            rows = window(); rows[-1]["device"]["source"][field] += difference
            with self.subTest(field=field, difference=difference), self.assertRaises(ValueError): a.ledger(rows)
        rows = window(); rows[-1]["device"]["source"]["overruns"] = 72745
        with self.assertRaises(ValueError): a.ledger(rows)

    def test_counted_losses_and_ring_drops_are_distinct(self):
        rows = window(); end = rows[-1]["device"]["source"]
        end.update(reads=end["reads"]-5, missed=5, i2c=1, faults=1, overruns=3)
        r = a.ledger(rows)
        self.assertEqual(r["source_miss_fraction"], 5/71744)
        self.assertEqual(r["source_delta"]["overruns"], 3)
        self.assertEqual(r["window_partition_residual"], 0)

    def test_lifetime_retention_exact_large_integers(self):
        rows = window()
        for row in rows:
            for key in ("reads", "intervals"):
                row["device"]["source"][key] += 1 << 63
        self.assertEqual(a.ledger(rows)["source_delta"]["intervals"], 71744)

    def test_saturation_regression_stall_and_stale(self):
        for mutate in (
            lambda r: r[-1]["device"]["source"].update(intervals=a.UINT64_MAX),
            lambda r: r[-1]["device"]["source"].update(reads=2, intervals=2),
            lambda r: r[-1]["device"].update(source_snapshot_fresh=False),
            lambda r: r[-1]["device"]["source"].update(missed=True),
            lambda r: r[-1]["device"].update(uptime_us=r[0]["device"]["uptime_us"]),
            lambda r: r[-1].update(monotonic_ns=r[0]["monotonic_ns"]),
        ):
            rows = window(); mutate(rows)
            with self.assertRaises(ValueError): a.ledger(rows)
        rows = window(); rows[1]["device"]["source"] = deepcopy(rows[0]["device"]["source"])
        with self.assertRaises(ValueError): a.ledger(rows)

    def test_throttling_tripwires_not_loss_allowances(self):
        rows = window()
        for row in rows:
            for key in ("reads", "intervals"):
                row["device"]["source"][key] //= 2
        r = a.ledger(rows)
        self.assertEqual(r["source_miss_fraction"], 0)
        self.assertTrue(any("actual interval cadence" in x for x in r["review"]))
        # Also expose a self-consistent half-speed calibration under MEDIUM.
        for row in rows: row["device"]["source_rate_millihz"] //= 2
        r = a.ledger(rows)
        self.assertTrue(any("nominal reference" in x for x in r["review"]))
        self.assertEqual(r["source_delta"]["intervals"], 35872)


class ShadowReference:
    """Only QST 13.2.3 shadow/drop semantics, not a driver or DRDY model.

    The conversion counter is external synthetic ground truth. We deliberately
    do NOT assume that a QMI timestamp or interrupt exposes locked-out updates.
    Command setup, AHB gating and lock delay are preconditions, not simulated.
    """
    def __init__(self):
        self.value = 0
        self.locked = False
        self.conversions = self.dropped = 0
        self.generation = 0

    def update(self, value):
        self.conversions += 1
        if self.locked:
            self.dropped += 1
        else:
            self.value = value & 0xffff

    def status_available_read(self):
        self.locked = True

    def read(self, register):
        value = self.value
        if register == 0x40: self.locked = False  # gyro is enabled
        return value & 255 if register == 0x39 else value >> 8 if register == 0x3a else 0

    def reset(self):
        self.generation += 1
        self.value = 0
        self.locked = False
        # External ground-truth loss is lifetime data; reset does not erase it.


class VendorReferenceTests(unittest.TestCase):
    def test_update_between_bytes_tears_without_lock(self):
        q = ShadowReference(); q.update(0x00ff)
        lo = q.read(0x39); q.update(0x0100); hi = q.read(0x3a)
        self.assertEqual((hi << 8) | lo, 0x01ff)
        self.assertNotIn(0x01ff, (0x00ff, 0x0100))

    def test_lock_preserves_sample_but_discards_conversion(self):
        q = ShadowReference(); q.update(0x00ff); q.status_available_read()
        lo = q.read(0x39); q.update(0x0100); hi = q.read(0x3a)
        self.assertEqual((hi << 8) | lo, 0x00ff)
        self.assertTrue(q.locked)  # AZ_H does not release 6DOF locking
        q.update(0x0200)
        self.assertEqual(q.dropped, 2)
        q.read(0x40); self.assertFalse(q.locked)
        q.update(0x0300); self.assertEqual(q.value, 0x0300)
        self.assertEqual(q.conversions, 4)
        # Calling the one coherent read the denominator would hide 2/4 loss.
        self.assertEqual(q.dropped / q.conversions, .5)

    def test_stalled_lock_reset_does_not_resurrect_old_sample_or_erase_loss(self):
        q = ShadowReference(); q.update(0x1234); q.status_available_read()
        generation = q.generation
        for i in range(100): q.update(i)
        self.assertEqual(q.dropped, 100)
        q.reset()
        self.assertNotEqual(generation, q.generation)
        self.assertFalse(q.locked)
        self.assertEqual(q.value, 0)
        self.assertEqual(q.dropped, 100)
        q.update(0x8000); self.assertEqual(q.value, 0x8000)

    def test_sparse_wire_capacity_against_external_conversion_clock(self):
        # Even ideal back-to-back reads cannot match all independent updates.
        seconds = 600
        conversions = int(Fraction(3569987 * seconds, 1000))
        max_reads = int(Fraction(400000 * seconds, 117))
        missed = conversions - max_reads
        self.assertGreater(missed * 10000, conversions)
        self.assertGreater(missed / conversions, .04)

    def test_fifo_read_pause_discards_external_updates(self):
        period = Fraction(1000000000, 3569987)
        read_us = Fraction((3 + 128 * 12) * 9 * 1000000, 400000)
        # Best-phase integer bound while the FIFO is in read mode. There is no
        # synthetic concurrent fill to make a model falsely promise no loss.
        dropped = int(read_us / period)
        self.assertGreaterEqual(dropped, 123)
        self.assertGreater(dropped / (128 + dropped), .49)


if __name__ == "__main__":
    unittest.main()
