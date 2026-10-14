from array import array
from dataclasses import asdict, replace
from pathlib import Path
import json
import math
import random
import struct
import tempfile
import threading
import types
import unittest
import wave
from unittest.mock import patch

from contactmic.protocol import (AudioFrame, ProtocolError, decode_audio, decode_status,
                                 control_request, control_response, descriptor_parts)
from contactmic.stream import Diagnostics, StreamTracker
from contactmic.dsp import DCBlocker, LinearResampler, Pipeline
from contactmic.audio import AudioFIFO, AudioError, WavSink, LiveSink, list_audio_devices

VECTORS = Path(__file__).resolve().parents[2] / "tests/vectors/v1"
def vector(name): return bytes.fromhex((VECTORS / f"{name}.hex").read_text())
def frame(seq=0,index=0,samples=(0,100,200),rate=7174400,flags=0,desc=2):
    return AudioFrame(seq,index,rate,flags,desc,tuple(samples))


class ProtocolTests(unittest.TestCase):
    def test_canonical_audio_vectors(self):
        manifest=json.loads((VECTORS/'manifest.json').read_text())
        for name, expected in manifest['audio'].items():
            with self.subTest(name=name):
                actual=asdict(decode_audio(vector(name)));actual['samples']=list(actual['samples'])
                self.assertEqual(actual,expected)

    def test_every_truncated_frame(self):
        data=vector('audio_wrap_flags')
        for n in range(len(data)):
            with self.subTest(length=n), self.assertRaises(ProtocolError):decode_audio(data[:n])
        with self.assertRaises(ProtocolError):decode_audio(data+b'\0')

    def test_reserved_version_flags_descriptor_count_and_rate(self):
        for offset,value in ((0,2),(1,8),(12,0),(12,255),(13,3),(13,8),(14,1),(15,1)):
            data=bytearray(vector('audio_wrap_flags'));data[offset]=value
            with self.subTest(offset=offset,value=value),self.assertRaises(ProtocolError):decode_audio(data)
        data=bytearray(vector('audio_min'));data[8:12]=b'\0'*4
        with self.assertRaises(ProtocolError):decode_audio(data)

    def test_descriptor_all_values(self):
        for value in range(256):
            if value in (0,1,2,4,5,6):self.assertEqual(descriptor_parts(value),(value&3,4 if value&4 else 2))
            else:
                with self.assertRaises(ProtocolError):descriptor_parts(value)

    def test_status_vectors(self):
        full=decode_status(vector('status_full'));event=decode_status(vector('status_event'))
        self.assertEqual(full.session_id,0x12345678);self.assertEqual(full.counters,(1,2,3,4,5))
        self.assertEqual(full.data_length,251);self.assertIsNone(event.counters)
        self.assertEqual(replace(full,counters=None,data_length=0),event)

    def test_status_malformed(self):
        original=vector('status_full')
        for offset,value in ((0,2),(1,1),(2,20),(14,3),(15,4),(16,3),(17,3),(18,4),(19,1),(62,1)):
            data=bytearray(original);data[offset]=value
            with self.subTest(offset=offset),self.assertRaises(ProtocolError):decode_status(data)
        for n in (0,19,21,63,65):
            with self.assertRaises(ProtocolError):decode_status(original[:n]+(b'\0' if n==65 else b''))
        data=bytearray(original);data[4:8]=b'\0'*4
        with self.assertRaises(ProtocolError):decode_status(data)

    def test_controls_and_response_vectors(self):
        self.assertEqual(control_request(3,0x1234,2),vector('control_set_axis'))
        self.assertEqual(control_request(9,0x1235,0x12345678),vector('control_commit'))
        code,payload=control_response(vector('response_prepared'),6,0x1234)
        self.assertEqual(code,0);self.assertEqual(payload,vector('status_full'))
        for op in (1,2,6,7,8):self.assertEqual(len(control_request(op,65535)),8)
        for args in ((0,0),(10,0),(1,-1),(1,65536),(3,0,3),(4,0,8),(5,0,4),(9,0,0),(1,0,1)):
            with self.subTest(args=args),self.assertRaises(ProtocolError):control_request(*args)

    def test_response_rejects_mismatch_and_reserved(self):
        for offset,value in ((0,2),(1,0x87),(2,0),(4,7),(5,1),(6,20)):
            data=bytearray(vector('response_prepared'));data[offset]=value
            with self.assertRaises(ProtocolError):control_response(bytes(data),6,0x1234)


class StreamTests(unittest.TestCase):
    def test_exact_source_gaps_separate_from_packet_loss(self):
        d=Diagnostics();t=StreamTracker(d)
        self.assertEqual(t.accept(frame()),0)
        self.assertEqual(t.accept(frame(1,8,flags=7)),5)
        self.assertEqual(d.packet_gaps,0);self.assertEqual(d.source_gaps,5)
        self.assertEqual(d.i2c_flag_packets,1);self.assertEqual(d.ring_flag_packets,1)
        self.assertEqual(t.accept(frame(3,14)),3);self.assertEqual(d.packet_gaps,1)

    def test_wrap_duplicates_reordered_and_overlap(self):
        d=Diagnostics();t=StreamTracker(d)
        t.accept(frame(65535,0xfffffffe,samples=(1,2,3)))
        before=d.source_gaps
        self.assertEqual(t.accept(frame(0,1)),0)
        self.assertIsNone(t.accept(frame(0,1)))
        self.assertIsNone(t.accept(frame(65535,0xfffffffe)))
        self.assertIsNone(t.accept(frame(1,2)))
        self.assertEqual(d.source_gaps,before);self.assertEqual(d.duplicate_or_reordered,3)
        self.assertEqual(t.accept(frame(1,4)),0)

    def test_session_reset_preserves_diagnostics_not_history(self):
        d=Diagnostics();t=StreamTracker(d);t.accept(frame(10,30));t.reset()
        self.assertEqual(t.accept(frame(0,0,rate=896800,desc=4)),0)
        self.assertEqual(d.packet_gaps,10);self.assertEqual(d.source_gaps,30)

    def test_metadata_change_rejected_without_progress(self):
        t=StreamTracker(Diagnostics());t.accept(frame())
        for changed in (frame(1,3,rate=896800),frame(1,3,desc=6)):
            with self.assertRaises(ProtocolError):t.accept(changed)
        self.assertEqual(t.accept(frame(1,3)),0)


class DspTests(unittest.TestCase):
    def test_dc_blocker_and_bypass(self):
        f=DCBlocker(7174400,20);values=[f.sample(0.25) for _ in range(3000)]
        self.assertAlmostEqual(values[0],0.25);self.assertLess(abs(values[-1]),1e-10)
        f.reset();self.assertAlmostEqual(f.sample(0.25),0.25)
        f=DCBlocker(896800,0);self.assertEqual([f.sample(x) for x in (0.1,0.4,-0.3)],[0.1,0.4,-0.3])

    def test_resampler_partition_invariance(self):
        random.seed(17);samples=[math.sin(i/15)*0.8 for i in range(901)]
        for rate in (896800,3587200,7174400,48000000,100000):
            with self.subTest(rate=rate):
                whole=LinearResampler(rate).push(samples);f=LinearResampler(rate);pieces=array('f');i=0
                while i<len(samples):
                    n=random.randint(1,114);pieces.extend(f.push(samples[i:i+n]));i+=n
                self.assertEqual(whole.tobytes(),pieces.tobytes())
                self.assertEqual(len(whole),math.ceil((len(samples)-1)*48000000/rate))

    def test_interpolator_exact_ramp(self):
        self.assertEqual(list(LinearResampler(24000000).push([0,1,0])),[0,0.5,1,0.5])
        self.assertEqual(list(LinearResampler(48000000).push([0,0.5,1])),[0,0.5])

    def test_resampler_invalid_batch_is_atomic(self):
        f=LinearResampler(7174400);f.push([0,0.2]);state=(f.previous,f.phase)
        for samples in ([0]*1025,[0,float('nan')],[2]):
            with self.assertRaises(ValueError):f.push(samples)
            self.assertEqual((f.previous,f.phase),state)

    def test_pipeline_partition_invariance(self):
        samples=[int(math.sin(i/30)*10000) for i in range(500)]
        def run(chunks):
            p=Pipeline();out=array('f');index=0
            for seq,n in enumerate(chunks):out.extend(p.process(frame(seq,index,samples[index:index+n])));index+=n
            return out
        self.assertEqual(run([250,250]).tobytes(),run([1,99,114,114,172]).tobytes())

    def test_short_concealment_and_large_gap_caps(self):
        p=Pipeline(cutoff_hz=0);p.process(frame(samples=(1000,1000,1000)))
        out=p.process(frame(1,5));self.assertEqual(p.diagnostics.concealed_samples,2)
        self.assertEqual(p.diagnostics.source_gaps,2);self.assertGreater(len(out),0)
        out=p.process(frame(2,1000000000));self.assertLess(len(out),100)
        self.assertEqual(p.diagnostics.large_gap_resets,1)
        self.assertGreater(p.diagnostics.source_gaps,100000000)

    def test_clipping_raw_and_reset(self):
        p=Pipeline(gain=10000,cutoff_hz=0);out=p.process(frame(samples=(32767,-32768,32767)))
        self.assertTrue(all(-1<=x<=1 for x in out));self.assertEqual(p.clipped_samples,3)
        p.reset();self.assertGreater(len(p.process(frame(rate=896800))),0)
        raw=Pipeline(gain=10000,raw=True);raw.process(frame(samples=(100,100,100)))
        self.assertEqual(raw.clipped_samples,0)

    def test_invalid_configuration_and_source_rates(self):
        for kwargs in ({'gain':float('nan')},{'gain':-1},{'conceal_ms':21},{'cutoff_hz':-1}):
            with self.assertRaises(ValueError):Pipeline(**kwargs)
        for rate in (0,99999,48000001):
            with self.assertRaises(ProtocolError):LinearResampler(rate)
        with self.assertRaises(ValueError):DCBlocker(100000,50)
        with self.assertRaises(ProtocolError):Pipeline().process(frame(samples=()))


class AudioTests(unittest.TestCase):
    def test_fifo_order_overflow_underflow(self):
        f=AudioFIFO(4);f.write([0,0.1,0.2]);self.assertEqual(len(f),3)
        f.write([0.3,0.4,0.5]);self.assertEqual(f.overrun_samples,2)
        result=list(f.read(6));self.assertEqual(len(result),6)
        self.assertAlmostEqual(result[0],0.2);self.assertAlmostEqual(result[3],0.5)
        self.assertEqual(result[-2:],[0,0]);self.assertEqual(f.underrun_samples,2)
        f.write([0]*10);self.assertEqual(len(f),4);self.assertEqual(f.overrun_samples,8)
        f.clear();self.assertEqual(len(f),0)

    def test_callback_lock_never_waits(self):
        f=AudioFIFO(4);f.write([0.5]*4)
        with f._lock:
            self.assertEqual(list(f.read(4)),[0]*4)
        self.assertEqual(f.contention_underruns,1);self.assertEqual(f.underrun_samples,4)
        self.assertEqual(len(f),4)

    def test_fifo_invalid_write_preserves_queue(self):
        f=AudioFIFO(4);f.write([0.2])
        for samples in ([float('inf')],[2],(0 for _ in range(262145))):
            with self.assertRaises(ValueError):f.write(samples)
            self.assertEqual(len(f),1)
        for count in (-1,48001):
            with self.assertRaises(ValueError):f.read(count)

    def test_wav_header_pcm_and_exclusive_creation(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'a.wav'
            with WavSink(path) as sink:sink.write([-1,0,1,0.5])
            with wave.open(str(path),'rb') as wav:
                self.assertEqual((wav.getnchannels(),wav.getsampwidth(),wav.getframerate(),wav.getnframes()),(1,2,48000,4))
                self.assertEqual(struct.unpack('<4h',wav.readframes(4)),(-32768,0,32767,16384))
            original=path.read_bytes()
            with self.assertRaises(AudioError):WavSink(path)
            self.assertEqual(path.read_bytes(),original)
            with WavSink(path,overwrite=True) as sink:sink.write([0])
            with self.assertRaises(AudioError):sink.write([0])

    def test_wav_invalid_batch_writes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'a.wav'
            with WavSink(path) as sink:
                with self.assertRaises(ValueError):sink.write([0,float('nan')])
            with wave.open(str(path),'rb') as wav:self.assertEqual(wav.getnframes(),0)

    def test_live_sink_uses_fake_backend_and_no_hardware(self):
        class Stream:
            def __init__(self,**kw):self.kw=kw;self.started=self.closed=False
            def start(self):self.started=True
            def stop(self):self.started=False
            def close(self):self.closed=True
        backend=types.SimpleNamespace(RawOutputStream=Stream,query_devices=lambda:['fake'])
        f=AudioFIFO(8);sink=LiveSink(f,backend=backend);stream=sink._stream
        self.assertTrue(stream.started);self.assertEqual(stream.kw['dtype'],'float32')
        sink.write([0.5]*4);out=bytearray(16);sink._callback(out,4,None,types.SimpleNamespace(output_underflow=True))
        self.assertEqual(list(array('f',out)),[0.5]*4);self.assertEqual(sink.backend_underflows,1)
        sink.close();sink.close();self.assertTrue(stream.closed)
        self.assertEqual(list_audio_devices(backend),['fake'])

    def test_actionable_output_failure(self):
        def fail(**kw):raise RuntimeError('no device')
        with self.assertRaisesRegex(AudioError,'48 kHz mono'):
            LiveSink(AudioFIFO(),backend=types.SimpleNamespace(RawOutputStream=fail))
