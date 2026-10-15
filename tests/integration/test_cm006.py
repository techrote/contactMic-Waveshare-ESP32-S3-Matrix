"""Production C acquisition/transport -> installed Python decoder/DSP qualification.

The simulator's only replacements are clock, sensor backend and notification
sink. This test adds receiver-side packet faults and slow audio consumption.
No Bluetooth, audio hardware or ESP32 is used. The output is reproducible, not
physical throughput, contact response, latency or RF acceptance evidence.
"""
from array import array
import argparse
from dataclasses import replace
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import wave

from contactmic import protocol, dsp, audio

SIMULATOR: Path


class EndToEndTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        results = []
        for _ in range(2):
            run = subprocess.run([str(SIMULATOR)], capture_output=True, check=True, timeout=30)
            if len(run.stdout) > 1000000:
                raise AssertionError("simulator exceeded the bounded evidence budget")
            results.append(run.stdout)
        if results[0] != results[1]:
            raise AssertionError("simulation output is not byte-for-byte reproducible")
        cls.events = [json.loads(line) for line in results[0].splitlines()]
        if len(cls.events) != 33:
            raise AssertionError("unexpected scenario output coverage")

    def events_for(self, scenario, event):
        return [item for item in self.events if (item["scenario"], item["event"]) == (scenario, event)]

    def frames(self, scenario):
        return [protocol.decode_audio(bytes.fromhex(e["audio"]))
                for e in self.events_for(scenario, "audio")]

    def status(self, scenario, event):
        return protocol.decode_status(bytes.fromhex(self.events_for(scenario, event)[0]["status"]))

    def test_loss_accounting_through_actual_firmware(self):
        pipeline = dsp.Pipeline()
        session = self.status("faults", "session")
        self.assertEqual(session.rate_millihz, 7200000)  # measured synthetic clock, not nominal ODR
        for frame in self.frames("faults"):
            self.assertEqual(frame.rate_millihz, session.rate_millihz)
            self.assertEqual(frame.descriptor, session.descriptor)
            self.assertLessEqual(len(frame.samples), 114)
            self.assertLessEqual(len(pipeline.process(frame)), 48000)
        diagnostics = pipeline.diagnostics
        self.assertEqual(diagnostics.accepted_packets, 6)
        self.assertEqual(diagnostics.source_gaps, 162)  # 3 DRDY + 1 I2C + 114 rejected + 44 ring
        self.assertEqual(diagnostics.packet_gaps, 1)
        self.assertGreater(diagnostics.i2c_flag_packets, 0)
        self.assertGreater(diagnostics.ring_flag_packets, 0)
        self.assertEqual(self.status("faults", "end").counters, (4, 1, 44, 1, 6))
        self.assertEqual(len(self.events_for("faults", "rejected")), 1)

    def test_host_packet_loss_duplicate_and_reordering(self):
        pipeline = dsp.Pipeline()
        frames = self.frames("faults")
        # Drop the one-sample second packet; replay stale packets after C arrives.
        sequence = [frames[0], frames[2], frames[0], frames[2], frames[1], *frames[3:]]
        for frame in sequence:
            pipeline.process(frame)
        d = pipeline.diagnostics
        self.assertEqual(d.source_gaps, 163)
        self.assertEqual(d.packet_gaps, 2)
        self.assertEqual(d.duplicate_or_reordered, 3)
        self.assertEqual(d.accepted_packets, 5)

    def test_reconnect_new_axis_range_rate_excludes_old_samples(self):
        pipeline = dsp.Pipeline()
        for frame in self.frames("faults"):
            pipeline.process(frame)
        before = pipeline.diagnostics.source_gaps
        old = self.status("faults", "session")
        new = self.status("reconnect", "session")
        self.assertGreater(new.session_id, old.session_id)
        self.assertEqual((new.descriptor, new.rate_millihz), (4, 3600000))
        frame = self.frames("reconnect")[0]
        self.assertEqual((frame.sequence, frame.first_index), (0, 0))
        with self.assertRaises(protocol.ProtocolError):
            pipeline.process(frame)  # rate changes require an explicit boundary
        pipeline.reset()
        pipeline.process(frame)
        self.assertEqual(pipeline.diagnostics.source_gaps, before)
        self.assertEqual(self.status("reconnect", "end").counters[:4], (4, 1, 44, 1))

    def test_modular_wrap_and_large_gap_resource_bound(self):
        frames = self.frames("wrap")
        self.assertEqual([f.sequence for f in frames], [65534, 65535, 0])
        self.assertEqual([f.first_index for f in frames], [0xfffffffd, 0, 2])
        pipeline = dsp.Pipeline()
        for frame in frames:
            self.assertLess(len(pipeline.process(frame)), 128)
        self.assertEqual(pipeline.diagnostics.source_gaps, 0xfffffffd)
        self.assertEqual(pipeline.diagnostics.packet_gaps, 65534)
        self.assertEqual(pipeline.diagnostics.duplicate_or_reordered, 0)
        self.assertEqual(pipeline.diagnostics.large_gap_resets, 1)

    def test_resampler_continuity_from_real_packetizer(self):
        frames = self.frames("clean")
        self.assertEqual([len(f.samples) for f in frames], [1, 7, 31, 2, 114, 5])
        partitioned, whole = dsp.Pipeline(), dsp.Pipeline()
        received = array("f")
        for frame in frames:
            received.extend(partitioned.process(frame))
        combined = replace(frames[0], samples=tuple(x for f in frames for x in f.samples))
        self.assertEqual(received.tobytes(), whole.process(combined).tobytes())
        self.assertEqual(partitioned.diagnostics.source_gaps, 0)
        self.assertEqual(partitioned.diagnostics.packet_gaps, 0)

    def test_slow_audio_sink_and_wav(self):
        fifo, pipeline = audio.AudioFIFO(128), dsp.Pipeline()
        rendered = array("f")
        for frame in self.frames("clean"):
            values = pipeline.process(frame)
            fifo.write(values)
            rendered.extend(values)
            self.assertLessEqual(len(fifo), 128)
        self.assertEqual(fifo.overrun_samples, len(rendered) - 128)
        self.assertEqual(fifo.read(128).tobytes(), rendered[-128:].tobytes())
        self.assertTrue(all(v == 0 for v in fifo.read(64)))
        self.assertEqual(fifo.underrun_samples, 64)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.wav"
            with audio.WavSink(path) as sink:
                sink.write(rendered)
            with wave.open(str(path), "rb") as source:
                self.assertEqual((source.getframerate(), source.getnchannels(), source.getsampwidth()), (48000, 1, 2))
                self.assertEqual(source.getnframes(), len(rendered))
                self.assertEqual(len(source.readframes(len(rendered))), 2 * len(rendered))

    def test_adaptive_boundaries_and_capacity_refusal(self):
        previous = 0
        for name, current, following in (("auto_high", 2, 1), ("auto_medium", 1, 0), ("auto_safe", 0, 0)):
            active, stopped = self.status(name, "session"), self.status(name, "boundary")
            self.assertGreater(active.session_id, previous)
            previous = active.session_id
            self.assertEqual((active.policy, active.state, active.selected_mode), (3, 2, current))
            self.assertEqual((stopped.state, stopped.rate_millihz, stopped.selected_mode), (0, 0, following))
            self.assertGreaterEqual(stopped.counters[1], 1)
        refused = self.status("auto_safe", "capacity_refused")
        self.assertEqual((refused.mtu, refused.state), (23, 0))
        self.assertEqual(refused.counters[1:4], (1, 0, 1))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", required=True, type=Path)
    options, rest = parser.parse_known_args()
    SIMULATOR = options.simulator.resolve(strict=True)
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
