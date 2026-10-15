"""Installed-package USB evidence tests; no real audio, serial or Bluetooth."""
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import json
import tempfile
import unittest
from unittest.mock import patch

from contactmic import usb_qualification as u

SERIAL = "AABBCCDDEEFF"


def device(second=0):
    return dict(schema=u.SCHEMA, request_id=1, result=0, serial=SERIAL, boot_id=100, reset_reason=1,
        firmware="a"*40, elf_sha256="b"*64, generation=7, state=3, reason=0, mode=2,
        source_rate_millihz=7174400, uac_rate=48000, axis=2, range_g=2, policy=2,
        gain_milli=1000, cutoff_millihz=20000, uptime_us=10000000+second*1000000,
        output_position=100000+second*48000, queue=144, queue_high=150, queue_low=0,
        source_available=True, source_snapshot_fresh=True, qmi={"address": 106, "who": 5, "revision": 104},
        source={key: (1000+second*7174 if key in ("intervals", "reads") else 0) for key in u.SOURCE_KEYS},
        usb={key: (100000+second*48000 if key=="output_samples" else 100 if key=="startup_silence" else 0)
             for key in u.USB_KEYS})


def evidence(seconds=600):
    metadata = dict(schema="cm009.capture.v1", serial=SERIAL, error=None, cleanup_errors=[],
        config=dict(axis=2, range_g=2, policy=2, gain_milli=1000, cutoff_millihz=20000,
                    duration_s=seconds, purpose="sustained", led_state="off"))
    rows = [dict(monotonic_ns=(10+i)*1000000000, phase="baseline" if i==0 else "final" if i==seconds else "steady",
             device=device(i), capture=dict(frames=48000*(i+1), callbacks=100*(i+1),
                                            overflows=0, queue_drops=0, bad_blocks=0)) for i in range(seconds+1)]
    return metadata, rows


class EvaluationTests(unittest.TestCase):
    def test_complete_window_not_whole_issue(self):
        meta, rows = evidence()
        result = u.evaluate(meta, rows)
        self.assertEqual(result["result"], "pass")
        self.assertFalse(result["issue_accepted"])
        self.assertEqual(result["source"]["intervals"], 600*7174)
        self.assertEqual(result["durations"]["host_audio_s"], 600)

    def test_source_denominator_is_not_48k(self):
        meta, rows = evidence()
        rows[-1]["device"]["source"]["missed"] = 5000
        # 5000 / 28.8 million USB samples would pass; actual source denominator must fail.
        self.assertEqual(u.evaluate(meta, rows)["result"], "fail")

    def test_high_i2c_review_medium_fail(self):
        meta, rows = evidence()
        rows[-1]["device"]["source"]["i2c"] = 1
        self.assertEqual(u.evaluate(meta, rows)["result"], "review_required")
        meta["config"]["policy"] = 1
        for row in rows:
            row["device"]["policy"] = row["device"]["mode"] = 1
        self.assertEqual(u.evaluate(meta, rows)["result"], "fail")

    def test_missing_short_regression_reset_cannot_pass(self):
        meta, base = evidence()
        mutations = [
            lambda r: r[-1]["device"]["source"].pop("intervals"),
            lambda r: r[300]["device"].update(source_snapshot_fresh=False),
            lambda r: r[-1]["device"]["usb"].update(clock_updates=(1<<64)-1),
            lambda r: r[-1]["device"].update(boot_id=200),
            lambda r: r[-1]["device"].update(generation=8),
            lambda r: r[-1]["device"]["source"].update(reads=1),
            lambda r: r[-1]["capture"].update(frames=r[0]["capture"]["frames"]+48000*599),
            lambda r: r[300]["device"].update(state=2),
            lambda r: r[300]["device"].update(source_rate_millihz=8000000),
            lambda r: r[300]["device"].update(serial="112233445566"),
            lambda r: r[-1].update(phase="steady"),
            lambda r: r[300].update(monotonic_ns=r[299]["monotonic_ns"]),
            lambda r: r[300]["device"]["usb"].update(clock_updates=-1),
            lambda r: r[300]["device"]["source"].update(missed=False),
        ]
        for change in mutations:
            with self.subTest(change=change):
                rows = deepcopy(base); change(rows)
                self.assertEqual(u.evaluate(meta, rows)["result"], "incomplete")
        self.assertEqual(u.evaluate(meta, base[:100]+base[110:])["result"], "incomplete")

    def test_unreviewed_loss_and_output_failure(self):
        meta, base = evidence()
        for field in ("queue_discards", "late_frames", "clock_saturated", "raw_clips"):
            rows = deepcopy(base); rows[-1]["device"]["usb"][field] = 1
            self.assertEqual(u.evaluate(meta, rows)["result"], "review_required")
        for field in ("starvation_silence", "failed_silence", "short_writes"):
            rows = deepcopy(base); rows[-1]["device"]["usb"][field] = 1
            self.assertEqual(u.evaluate(meta, rows)["result"], "fail")
        for field in ("overflows", "queue_drops", "bad_blocks"):
            rows = deepcopy(base); rows[-1]["capture"][field] = 1
            self.assertEqual(u.evaluate(meta, rows)["result"], "fail")

    def test_operational_and_cleanup_failure(self):
        meta, rows = evidence()
        meta["error"] = "abort"
        self.assertEqual(u.evaluate(meta, rows)["result"], "incomplete")
        meta["error"] = None; meta["cleanup_errors"] = ["USB unplugged"]
        self.assertEqual(u.evaluate(meta, rows)["result"], "incomplete")

    def test_manifest_verification_preserves_evidence(self):
        meta, rows = evidence()
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            u.dump(p/"metadata.json", meta)
            (p/"telemetry.jsonl").write_text("".join(json.dumps(r)+"\n" for r in rows), encoding="utf-8")
            u.dump(p/"SHA256SUMS.json", u.manifest(p))
            self.assertEqual(u.summarize(p)["result"], "pass")
            before = (p/"SHA256SUMS.json").read_bytes()
            (p/"metadata.json").write_text("{}", encoding="utf-8")
            with self.assertRaises(u.QualificationError):
                u.summarize(p)
            self.assertEqual((p/"SHA256SUMS.json").read_bytes(), before)


class InterfaceTests(unittest.TestCase):
    def setUp(self):
        self.port = SimpleNamespace(serial_number=SERIAL, vid=0xcafe, pid=0x4014, device="COM7")
        self.audio = [{"name": f"Microphone (CM USB {SERIAL})", "max_input_channels": 1, "hostapi": 0}]
        self.apis = [{"name": "Windows WASAPI"}]

    def test_exact_identity_and_ambiguity(self):
        self.assertEqual(u.choose_interfaces(SERIAL,[self.port],self.audio,self.apis)[1],0)
        with self.assertRaises(u.QualificationError):
            u.choose_interfaces(SERIAL,[self.port],self.audio*2,self.apis)
        self.assertEqual(u.choose_interfaces(SERIAL,[self.port],self.audio*2,self.apis,audio_index=1)[1],1)
        for name in ("Microphone (CM USB AABB)", "Other AABBCCDDEEFF", "CM USB 1AABBCCDDEEFF2"):
            with self.assertRaises(u.QualificationError):
                u.choose_interfaces(SERIAL,[self.port],[dict(self.audio[0],name=name)],self.apis)
        self.port.serial_number = "112233445566"
        with self.assertRaises(u.QualificationError):
            u.choose_interfaces(SERIAL,[self.port],self.audio,self.apis)

    def test_cdc_protocol_identity_lengths_and_fragments(self):
        class Port:
            def write(self, data): return len(data)
            def read_until(self, *a, **kw): return self.chunks.pop(0)
        p = Port(); payload = (json.dumps(device())+"\n").encode()
        p.chunks = [payload[:20],payload[20:]]
        self.assertEqual(u.ControlLink(p,SERIAL).request()["serial"],SERIAL)
        p.chunks = [payload]
        with self.assertRaises(u.QualificationError):
            u.ControlLink(p,"112233445566").request()
        d = device(); d["request_id"] = 2; p.chunks = [(json.dumps(d)+"\n").encode()]
        with self.assertRaises(u.QualificationError):
            u.ControlLink(p,SERIAL).request()
        p.chunks = [b"x"*4096]
        with self.assertRaises(u.QualificationError):
            u.ControlLink(p,SERIAL).request()

    def test_bounded_audio_callback(self):
        b = u.CaptureBuffer(True,2)
        status = SimpleNamespace(input_overflow=False)
        for _ in range(4): b.callback(b"\x00"*960,480,None,status)
        self.assertEqual(b.queue.qsize(),2)
        self.assertEqual(b.counts.frames,1920)
        self.assertEqual(b.counts.queue_drops,2)
        b.callback(b"",480,None,SimpleNamespace(input_overflow=True))
        self.assertEqual((b.counts.bad_blocks,b.counts.overflows),(1,1))

    def test_failed_capture_is_preserved_without_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            out=Path(directory)/"evidence"
            args=SimpleNamespace(duration=10,purpose="preflight",policy="safe",led_state="off",
                gain=1,high_pass=20,serial=SERIAL,port=None,audio_device=None,output=out,
                axis="z",range_g=2,note="fake test",wav=False)
            def fail(**kwargs): raise OSError("fake USB unavailable")
            modules=(SimpleNamespace(Serial=fail),SimpleNamespace(comports=lambda:[self.port]),
                     SimpleNamespace(query_devices=lambda:self.audio,query_hostapis=lambda:self.apis))
            with patch("builtins.print"):
                self.assertEqual(u.capture(args,modules=modules),2)
            self.assertIn("USB unavailable",json.loads((out/"metadata.json").read_text())["error"])
            self.assertEqual(u.summarize(out)["result"],"incomplete")
            before=(out/"SHA256SUMS.json").read_bytes()
            with self.assertRaises(FileExistsError): u.capture(args,modules=modules)
            self.assertEqual((out/"SHA256SUMS.json").read_bytes(),before)

    def test_capture_orchestration_with_virtual_usb_devices(self):
        clock = [0.0]
        active = [False]
        callback = [None]
        class Serial:
            def __init__(self, **kwargs): self.payload = b""
            def open(self): pass
            def close(self): pass
            def write(self, request):
                tokens=request.decode().split()
                d=device(int(clock[0])); d.update(request_id=int(tokens[1]), state=3 if active[0] else 0, mode=0, policy=0)
                self.payload=(json.dumps(d)+"\n").encode()
                return len(request)
            def read_until(self,*args,**kwargs):
                value=self.payload; self.payload=b""; return value
        class Stream:
            def __init__(self,**kwargs): callback[0]=kwargs["callback"]
            def start(self): active[0]=True
            def stop(self): active[0]=False
            def close(self): pass
        def sleep(seconds):
            clock[0]+=seconds
            if active[0]:
                for _ in range(int(seconds*100)):
                    callback[0](b"\0"*960,480,None,SimpleNamespace(input_overflow=False))
        audio=SimpleNamespace(query_devices=lambda:self.audio,query_hostapis=lambda:self.apis,RawInputStream=Stream)
        modules=(SimpleNamespace(Serial=Serial),SimpleNamespace(comports=lambda:[self.port]),audio)
        with tempfile.TemporaryDirectory() as directory:
            args=SimpleNamespace(duration=2,purpose="preflight",policy="safe",led_state="off",
                gain=1,high_pass=20,serial=SERIAL,port=None,audio_device=None,output=Path(directory)/"run",
                axis="z",range_g=2,note="explicit synthetic USB/clock",wav=False)
            with patch.object(u.time,"monotonic",side_effect=lambda:clock[0]), patch.object(u.time,"monotonic_ns",side_effect=lambda:int(clock[0]*1e9)), patch.object(u.time,"sleep",side_effect=sleep), patch("builtins.print"):
                self.assertEqual(u.capture(args,modules=modules),0)
            result=u.summarize(args.output)
            self.assertEqual(result["result"],"evidence_complete")
            self.assertFalse(result["issue_accepted"])
            self.assertEqual(result["durations"]["host_audio_s"],2)
            self.assertFalse(active[0])

    def test_help_no_hardware(self):
        with patch.object(u,"hardware_modules",side_effect=AssertionError("device access")), patch("builtins.print"):
            with self.assertRaises(SystemExit) as code: u.main(["--help"])
        self.assertEqual(code.exception.code,0)


if __name__ == "__main__":
    unittest.main()
