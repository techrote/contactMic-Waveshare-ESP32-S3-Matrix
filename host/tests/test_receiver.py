"""Deterministic fake BLE and output tests; no adapter, board or audio device."""
import asyncio
import struct
import threading
from types import SimpleNamespace
import unittest

from contactmic.dsp import Pipeline
from contactmic.protocol import AUDIO_UUID, CONTROL_UUID, STATUS_UUID, SERVICE_UUID, ProtocolError
from contactmic.receiver import (PacketIngress, Receiver, ReceiverConfig, ReceiverError,
                                 discover, select_device)


def packet(sequence=0, index=0, *, rate=896800, descriptor=2, count=8):
    return struct.pack('<BBHIIBBH', 1, 0, sequence, index, rate, count, descriptor, 0) + struct.pack(
        f'<{count}h', *[i * 100 for i in range(count)])


class FakeClient:
    """Reference peer uses explicit struct layouts, not production encoder helpers."""
    def __init__(self, device, *, disconnected_callback, timeout):
        self.device = device
        self.disconnected_callback = disconnected_callback
        self.timeout = timeout
        self.is_connected = False
        self.callbacks = {}
        self.saved_callbacks = {}
        self.controls = []
        self.session = 100
        self.state = 0
        self.axis = 2
        self.range_g = 2
        self.policy = 0
        self.rate = 896800
        self.reply = b''
        self.source_present = True
        self.busy_count = 0
        self.frames = [packet()]
        self.metadata_error = False
        self.disconnect_after_commit = False
        self.connect_error = None
        self.reply_error = False
        self.stopped_notifications = []
        self.disconnect_calls = 0
        self.committed_with_subscriber = False
        self.inject_old_status = True
        self.invalid_status_event = False
        self.prepare_wrong_config = False
        self.commit_result = 0

    def status(self, *, event=False, state=None):
        state = self.state if state is None else state
        header = struct.pack('<BBHIIHBBBBBB', 1, int(event), 20 if event else 64,
                             self.session, self.rate, 247,
                             self.axis | (4 if self.range_g == 4 else 0),
                             self.policy, state, 0, 0, 0)
        return header if event else header + struct.pack('<5QHH', 3, 2, 1, 4, 5, 0, 0)

    async def connect(self):
        if self.connect_error:
            raise self.connect_error
        self.is_connected = True

    async def disconnect(self):
        self.disconnect_calls += 1
        self.is_connected = False
        self.disconnected_callback(self)

    async def start_notify(self, uuid, callback):
        self.callbacks[uuid] = callback
        self.saved_callbacks[uuid] = callback
        if uuid == STATUS_UUID:
            callback(uuid, self.status(event=True))

    async def stop_notify(self, uuid):
        self.stopped_notifications.append(uuid)
        self.callbacks.pop(uuid, None)

    async def write_gatt_char(self, uuid, data, *, response):
        if uuid != CONTROL_UUID or response is not True:
            raise AssertionError('controls require the correct UUID and acknowledged writes')
        major, op, ident, n, reserved = struct.unpack_from('<BBHHH', data)
        if major != 1 or reserved or len(data) != n + 8:
            raise AssertionError('malformed host control')
        self.controls.append(op)
        result = 0
        if op == 3:
            self.axis = data[8]
        elif op == 4:
            self.range_g = data[8]
        elif op == 5:
            self.policy = data[8]
        elif op == 6:
            if self.busy_count:
                self.busy_count -= 1
                result = 4
            else:
                self.session += 1
                self.state = 1
                if self.prepare_wrong_config:
                    self.axis = (self.axis + 1) % 3
        elif op == 9:
            if struct.unpack_from('<I', data, 8)[0] != self.session:
                raise AssertionError('host committed a stale session')
            self.committed_with_subscriber = AUDIO_UUID in self.callbacks
            if not self.committed_with_subscriber:
                raise AssertionError('host must install audio callback before commit')
            result = self.commit_result
            self.state = 2 if not result else 0
            if not result:
                # Deliver callbacks BEFORE the corresponding control read completes.
                if self.inject_old_status:
                    self.callbacks[STATUS_UUID](STATUS_UUID, self.status(event=True, state=0))
                if self.invalid_status_event:
                    self.callbacks[STATUS_UUID](STATUS_UUID, b'bad')
                frames = [packet(rate=self.rate + 1)] if self.metadata_error else self.frames
                for frame in frames:
                    self.callbacks[AUDIO_UUID](AUDIO_UUID, frame)
                if self.disconnect_after_commit:
                    self.is_connected = False
                    self.disconnected_callback(self)
        elif op == 7:
            self.state = 0
        payload = (bytes([1, 1, 7, 3, 15, 7 if self.source_present else 3, 247, 0])
                   if op == 1 else self.status())
        self.reply = struct.pack('<BBHBBH', 1, op | 0x80, (ident + int(self.reply_error)) & 65535,
                                 result, 0, len(payload)) + payload

    async def read_gatt_char(self, uuid):
        if uuid == CONTROL_UUID:
            return self.reply
        if uuid == STATUS_UUID:
            return self.status()
        raise AssertionError('unexpected read UUID')


class Factory:
    def __init__(self, **settings):
        self.settings = settings
        self.clients = []

    def __call__(self, *args, **kwargs):
        client = FakeClient(*args, **kwargs)
        for key, value in self.settings.items():
            setattr(client, key, value)
        self.clients.append(client)
        return client


class MemorySink:
    def __init__(self):
        self.values = []

    def write(self, values):
        self.values.extend(values)


class ReceiverTests(unittest.IsolatedAsyncioTestCase):
    def receiver(self, **kwargs):
        return Receiver(Pipeline(cutoff_hz=0), ReceiverConfig(
            duration=0.07, io_timeout=0.2, prepare_timeout=0.12, idle_timeout=0.5, **kwargs))

    async def test_prepare_subscribe_commit_order_and_early_audio(self):
        receiver = self.receiver()
        factory = Factory()
        sink = MemorySink()
        report = await receiver.run('fake', client_factory=factory, sink=sink)
        client = factory.clients[0]
        self.assertEqual(client.controls, [1, 3, 4, 5, 6, 9, 7])
        self.assertTrue(client.committed_with_subscriber)
        self.assertEqual(report['host']['accepted_packets'], 1)
        self.assertGreater(len(sink.values), 300)
        self.assertEqual(receiver.status.state, 0)
        self.assertEqual(receiver.status.counters, (3, 2, 1, 4, 5))
        self.assertEqual(report['firmware']['counters']['i2c_errors'], 2)
        self.assertEqual(report['firmware']['axis'], 'z')
        self.assertEqual(set(client.stopped_notifications), {AUDIO_UUID, STATUS_UUID})
        self.assertEqual(client.disconnect_calls, 1)
        self.assertEqual(receiver.cleanup_errors, [])

    async def test_reconnect_and_old_callbacks_do_not_contaminate_new_session(self):
        receiver = self.receiver()
        factory = Factory()
        await receiver.run('fake', client_factory=factory)
        old = factory.clients[0].saved_callbacks[AUDIO_UUID]
        first_ingress = receiver.ingress
        old(AUDIO_UUID, packet(sequence=2, index=1000000))
        self.assertFalse(first_ingress.queue)
        second = asyncio.create_task(receiver.run('fake', client_factory=factory))
        await asyncio.sleep(0.01)
        old(AUDIO_UUID, packet(sequence=200, index=1000000))
        result = await second
        self.assertIsNot(first_ingress, receiver.ingress)
        self.assertEqual(result['host']['accepted_packets'], 2)
        self.assertEqual(result['host']['source_gaps'], 0)

    async def test_prepare_busy_then_ready(self):
        receiver = self.receiver()
        factory = Factory(busy_count=1)
        await receiver.run('fake', client_factory=factory)
        self.assertEqual(factory.clients[0].controls.count(6), 2)

    async def test_prepare_timeout_is_bounded_and_stops_source(self):
        receiver = self.receiver()
        factory = Factory(busy_count=100)
        with self.assertRaisesRegex(ReceiverError, 'preparation timed out'):
            await receiver.run('fake', client_factory=factory)
        self.assertIn(7, factory.clients[0].controls)
        self.assertLess(factory.clients[0].controls.count(6), 10)
        self.assertFalse(receiver._running)

    async def test_missing_source_is_explicit_and_never_committed(self):
        factory = Factory(source_present=False)
        with self.assertRaisesRegex(ReceiverError, 'CM-006'):
            await self.receiver().run('fake', client_factory=factory)
        self.assertNotIn(6, factory.clients[0].controls)
        self.assertEqual(factory.clients[0].disconnect_calls, 1)

    async def test_wrong_prepared_configuration_is_rejected(self):
        factory = Factory(prepare_wrong_config=True)
        with self.assertRaisesRegex(ReceiverError, 'configuration'):
            await self.receiver().run('fake', client_factory=factory)
        self.assertNotIn(9, factory.clients[0].controls)

    async def test_bad_control_response_identity_is_rejected(self):
        with self.assertRaisesRegex(ProtocolError, 'identity mismatch'):
            await self.receiver().run('fake', client_factory=Factory(reply_error=True))

    async def test_commit_failure_and_disconnect_are_actionable(self):
        for settings, expected in (({'commit_result': 5}, 'source I/O'),
                                    ({'disconnect_after_commit': True}, 'connection lost')):
            with self.subTest(settings=settings), self.assertRaisesRegex(ReceiverError, expected):
                await self.receiver().run('fake', client_factory=Factory(**settings))

    async def test_malformed_audio_drops_without_corrupting_good_frame(self):
        malformed = bytearray(packet())
        malformed[14] = 1
        receiver = self.receiver()
        result = await receiver.run('fake', client_factory=Factory(frames=[b'x', malformed, packet()]))
        self.assertEqual(result['host']['invalid_frames'], 2)
        self.assertEqual(result['host']['accepted_packets'], 1)

    async def test_audio_metadata_change_and_malformed_status_fail(self):
        for settings in ({'metadata_error': True}, {'invalid_status_event': True}):
            with self.subTest(settings=settings), self.assertRaises(ReceiverError):
                await self.receiver().run('fake', client_factory=Factory(**settings))

    async def test_no_audio_idle_timeout(self):
        receiver = Receiver(Pipeline(), ReceiverConfig(duration=1, idle_timeout=0.05))
        with self.assertRaisesRegex(ReceiverError, 'idle timeout'):
            await receiver.run('fake', client_factory=Factory(frames=[]))

    async def test_connect_error_still_cleans_up(self):
        factory = Factory(connect_error=OSError('adapter disabled'))
        with self.assertRaisesRegex(ReceiverError, 'adapter disabled'):
            await self.receiver().run('fake', client_factory=factory)
        self.assertEqual(factory.clients[0].disconnect_calls, 1)

    async def test_shutdown_joins_inflight_sink_write(self):
        entered, release, finished = threading.Event(), threading.Event(), threading.Event()
        class SlowSink:
            def write(self, _values):
                entered.set()
                if not release.wait(2):
                    raise RuntimeError('test failed to release sink')
                finished.set()
        receiver = Receiver(Pipeline(), ReceiverConfig(duration=None))
        task = asyncio.create_task(receiver.run('fake', client_factory=Factory(), sink=SlowSink()))
        try:
            for _ in range(100):
                if entered.is_set():
                    break
                await asyncio.sleep(0.002)
            self.assertTrue(entered.is_set())
            task.cancel()
            await asyncio.sleep(0.01)
            self.assertFalse(task.done(), 'must not close the sink while the worker owns it')
            task.cancel()
            await asyncio.sleep(0.01)
            self.assertFalse(task.done(), 'repeated cancellation must not detach the worker thread')
        finally:
            release.set()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(finished.is_set())
        self.assertFalse(receiver._running)

    async def test_sink_failure_is_not_reported_as_success(self):
        class BadSink:
            def write(self, _values):
                raise OSError('disk full')
        with self.assertRaisesRegex(ReceiverError, 'disk full'):
            await self.receiver().run('fake', client_factory=Factory(), sink=BadSink())

    async def test_ingress_wakeups_and_memory_are_bounded(self):
        real_loop = asyncio.get_running_loop()
        class Loop:
            calls = 0
            def call_soon_threadsafe(self, function):
                self.calls += 1
                real_loop.call_soon_threadsafe(function)
        loop = Loop()
        ingress = PacketIngress(loop, 4)
        for i in range(10000):
            ingress.submit(packet(sequence=i))
        self.assertEqual(len(ingress.queue), 4)
        self.assertEqual(ingress.dropped, 9996)
        self.assertEqual(loop.calls, 1)
        for i in range(9996, 10000):
            self.assertEqual(await ingress.next(), packet(sequence=i))
        waiter = asyncio.create_task(ingress.next())
        await asyncio.sleep(0)
        ingress.submit(packet())
        self.assertEqual(await waiter, packet())
        ingress.close()
        ingress.submit(packet())
        self.assertEqual(len(ingress.queue), 0)
        with self.assertRaises(asyncio.CancelledError):
            await ingress.next()

    async def test_discovery_filter_and_selection(self):
        a = SimpleNamespace(address='aa:01', name='S3-ContactMic-one')
        b = SimpleNamespace(address='aa:02', name=None)
        c = SimpleNamespace(address='aa:03', name='other')
        class Scanner:
            @staticmethod
            async def discover(**kwargs):
                if not kwargs['return_adv']:
                    raise AssertionError('advertisement data required')
                return {'a': (a, SimpleNamespace(local_name=None, service_uuids=[])),
                        'b': (b, SimpleNamespace(local_name=None, service_uuids=[SERVICE_UUID.upper()])),
                        'c': (c, SimpleNamespace(local_name='other', service_uuids=[]))}
        devices = await discover(0.1, scanner=Scanner)
        self.assertEqual(devices, [a, b])
        self.assertIs(select_device(devices, 'AA:02'), b)
        with self.assertRaisesRegex(ReceiverError, 'More than one'):
            select_device(devices)
        with self.assertRaisesRegex(ReceiverError, 'No matching'):
            select_device(devices, 'missing')
        with self.assertRaises(ValueError):
            await discover(float('nan'), scanner=Scanner)

    async def test_reject_invalid_config_before_access(self):
        for config in (ReceiverConfig(axis='q'), ReceiverConfig(range_g=8), ReceiverConfig(queue_packets=0),
                       ReceiverConfig(io_timeout=float('nan')), ReceiverConfig(duration=-1)):
            with self.subTest(config=config), self.assertRaises(ValueError):
                Receiver(Pipeline(), config)


if __name__ == '__main__':
    unittest.main()
