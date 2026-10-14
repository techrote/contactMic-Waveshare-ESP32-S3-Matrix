"""BLE session owner with bounded callback ingress and one serialized DSP worker."""
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, asdict
import asyncio
import math
import threading
import time

from .protocol import (AXES, POLICIES, SERVICE_UUID, AUDIO_UUID, CONTROL_UUID,
                       STATUS_UUID, NAME_PREFIX, ProtocolError, Status,
                       control_request, control_response, decode_audio, decode_status)
from .dsp import Pipeline, valid_rate


class ReceiverError(RuntimeError):
    """A device, transport, protocol-session or output error with operator context."""


def bleak_api():
    try:
        from bleak import BleakClient, BleakScanner
    except (ImportError, OSError) as exc:
        raise ReceiverError("BLE operation needs the 'ble' extra and a working Bluetooth adapter; "
                            "install with: python -m pip install './host[ble]'") from exc
    return BleakClient, BleakScanner


async def discover(timeout: float = 5, *, scanner=None) -> list:
    if not math.isfinite(timeout) or not 0 < timeout <= 120:
        raise ValueError("scan timeout must be greater than zero and at most 120 seconds")
    if scanner is None:
        _, scanner = bleak_api()
    try:
        found = await asyncio.wait_for(scanner.discover(timeout=timeout, return_adv=True), timeout + 5)
    except Exception as exc:
        raise ReceiverError(f"BLE discovery failed; check adapter power/permissions: {exc}") from exc
    return [device for device, advertisement in found.values()
            if SERVICE_UUID in [u.lower() for u in (advertisement.service_uuids or [])]
            or (advertisement.local_name or device.name or "").startswith(NAME_PREFIX)]


def select_device(devices: list, identifier: str | None = None):
    matches = devices if identifier is None else [d for d in devices
        if d.address.casefold() == identifier.casefold() or (d.name or "") == identifier]
    if not matches:
        raise ReceiverError("No matching ContactMic device found; check firmware, adapter and discovery")
    if len(matches) != 1:
        raise ReceiverError("More than one matching device; select its exact address with --device")
    return matches[0]


@dataclass(frozen=True, slots=True)
class ReceiverConfig:
    axis: str = "z"
    range_g: int = 2
    policy: str = "safe"
    duration: float | None = None
    io_timeout: float = 10
    prepare_timeout: float = 10
    idle_timeout: float = 5
    queue_packets: int = 32

    def validate(self) -> None:
        if self.axis not in AXES or self.range_g not in (2, 4) or self.policy not in POLICIES:
            raise ValueError("invalid source axis/range/policy")
        for name in ("io_timeout", "prepare_timeout", "idle_timeout"):
            value = getattr(self, name)
            if not math.isfinite(value) or not 0 < value <= 120:
                raise ValueError(f"{name} must be greater than zero and at most 120 seconds")
        if self.duration is not None and (not math.isfinite(self.duration) or self.duration <= 0):
            raise ValueError("duration must be finite and greater than zero")
        if not isinstance(self.queue_packets, int) or not 1 <= self.queue_packets <= 256:
            raise ValueError("callback queue must hold 1..256 packets")


class PacketIngress:
    """Thread-safe bounded ingress. Wakeups are coalesced, not queued per packet."""
    def __init__(self, loop, capacity: int):
        if not 1 <= capacity <= 256:
            raise ValueError("ingress capacity must be 1..256")
        self.loop = loop
        self.capacity = capacity
        self.queue: deque[bytes] = deque()
        self.lock = threading.Lock()
        self.event = asyncio.Event()
        self.active = True
        self.wake_pending = False
        self.dropped = 0
        self.invalid = 0

    def submit(self, data) -> None:
        # No decoding, DSP, device writes or disk I/O in the notification callback.
        if not 18 <= len(data) <= 526:
            with self.lock:
                if self.active:
                    self.invalid += 1
            return
        with self.lock:
            if not self.active:
                return
            if len(self.queue) == self.capacity:
                self.queue.popleft()
                self.dropped += 1
            self.queue.append(bytes(data))
            if not self.wake_pending:
                self.wake_pending = True
                self.loop.call_soon_threadsafe(self.event.set)

    async def next(self) -> bytes:
        while True:
            with self.lock:
                if self.queue:
                    return self.queue.popleft()
                if not self.active:
                    raise asyncio.CancelledError
                self.event.clear()
                self.wake_pending = False
            await self.event.wait()

    def close(self) -> None:
        with self.lock:
            self.active = False
            self.queue.clear()
        self.event.set()


class Receiver:
    """One run creates a fresh connection generation. Caller owns/cleans the sink.

    Disconnect ends this run rather than silently switching generations. Calling
    run again performs a fresh handshake and resets the pipeline before audio.
    """
    def __init__(self, pipeline: Pipeline, config: ReceiverConfig | None = None):
        self.pipeline = pipeline
        self.config = config or ReceiverConfig()
        self.config.validate()
        self.status: Status | None = None
        self._running = False
        self.ingress: PacketIngress | None = None
        self.last_valid_packet = 0.0
        self.cleanup_errors: list[str] = []
        self._request_id = 0
        self._control_lock: asyncio.Lock | None = None

    async def _control(self, client, opcode: int, value=None, *, allow_busy=False) -> tuple[int, bytes]:
        assert self._control_lock is not None
        async with self._control_lock:
            self._request_id = (self._request_id + 1) & 0xffff
            request_id = self._request_id
            data = control_request(opcode, request_id, value)
            await asyncio.wait_for(client.write_gatt_char(CONTROL_UUID, data, response=True), self.config.io_timeout)
            reply = await asyncio.wait_for(client.read_gatt_char(CONTROL_UUID), self.config.io_timeout)
            result, payload = control_response(reply, opcode, request_id)
            if result and not (allow_busy and result == 4):
                names = {1: "invalid value", 2: "unsupported/source unavailable", 3: "invalid session state",
                         4: "source busy", 5: "source I/O failure", 6: "insufficient capacity"}
                raise ReceiverError(f"Firmware rejected control {opcode}: {names[result]}. "
                                    "Check firmware integration and reconnect before restarting a committed stream.")
            return result, payload

    def diagnostics(self, sink=None) -> dict:
        result = {"host": asdict(self.pipeline.diagnostics), "clipped_samples": self.pipeline.clipped_samples}
        if self.ingress:
            result["host"]["callback_queue_drops"] = self.ingress.dropped
            result["host"]["invalid_frames"] += self.ingress.invalid
        if self.status:
            firmware = result["firmware"] = asdict(self.status)
            names = ("source_misses", "i2c_errors", "ring_overruns", "notify_rejections", "packets_sent")
            firmware["counters"] = (dict(zip(names, self.status.counters))
                                    if self.status.counters is not None else None)
            firmware["axis"] = ("x", "y", "z")[self.status.descriptor & 3]
            firmware["range_g"] = 4 if self.status.descriptor & 4 else 2
            firmware["policy_name"] = ("safe", "medium", "high", "auto")[self.status.policy]
            firmware["selected_mode_name"] = ("safe", "medium", "high")[self.status.selected_mode]
            capacity = max(1, min(255, (self.status.mtu - 19) // 2))
            result["notifications_per_second_required"] = self.status.rate_millihz / 1000 / capacity
            if self.status.mtu == 23:
                result["warning"] = "ATT MTU 23 carries only two samples; high-rate streaming is not a sustainable target."
        if sink is not None and hasattr(sink, "fifo"):
            result["audio"] = {"underrun_samples": sink.fifo.underrun_samples,
                               "overrun_samples": sink.fifo.overrun_samples,
                               "callback_failures": sink.callback_failures,
                               "backend_underflows": sink.backend_underflows}
        if self.cleanup_errors:
            result["cleanup_errors"] = self.cleanup_errors.copy()
        return result

    async def run(self, device, *, sink=None, client_factory=None, report: Callable[[dict], None] | None = None) -> dict:
        if self._running:
            raise ReceiverError("This receiver already owns an active connection")
        self._running = True
        self._control_lock = asyncio.Lock()
        self.pipeline.reset()
        self.status = None
        self.cleanup_errors = []
        loop = asyncio.get_running_loop()
        disconnected = asyncio.Event()
        active = True
        self.ingress = ingress = PacketIngress(loop, self.config.queue_packets)
        worker = None
        client = None
        audio_subscribed = status_subscribed = False
        session: Status | None = None
        callback_failure: str | None = None
        primary_failure = False
        worker_failure: Exception | None = None

        def on_disconnect(_client):
            if active:
                loop.call_soon_threadsafe(disconnected.set)

        def audio_callback(_sender, data):
            ingress.submit(data)  # Bound to this run, never to a later ingress.

        def status_callback(_sender, data):
            nonlocal callback_failure
            if not active:
                return
            try:
                observed = decode_status(data)
                # Events can be queued before the control response. Validate their
                # shape, but use serialized full reads for authoritative lifecycle.
                # A delayed initial STOPPED event must not invalidate preparation.
                del observed
            except ProtocolError as exc:
                callback_failure = str(exc)

        def process(data):
            try:
                frame = decode_audio(data)
            except ProtocolError:
                self.pipeline.diagnostics.invalid_frames += 1
                return
            if session is None or frame.rate_millihz != session.rate_millihz or frame.descriptor != session.descriptor:
                raise ReceiverError("Audio metadata differs from the prepared session; reconnect required")
            accepted = self.pipeline.diagnostics.accepted_packets
            values = self.pipeline.process(frame)
            if self.pipeline.diagnostics.accepted_packets != accepted:
                self.last_valid_packet = time.monotonic()
            if sink is not None and values:
                sink.write(values)

        async def consume():
            while True:
                data = await ingress.next()
                job = asyncio.create_task(asyncio.to_thread(process, data))
                try:
                    await asyncio.shield(job)
                except asyncio.CancelledError:
                    # A thread cannot be cancelled. Join before the caller closes a WAV/device.
                    while not job.done():
                        try:
                            await asyncio.shield(job)
                        except asyncio.CancelledError:
                            # A second cancellation must not detach the actual thread.
                            continue
                    job.result()
                    raise

        try:
            if client_factory is None:
                client_factory, _ = bleak_api()
            client = client_factory(device, disconnected_callback=on_disconnect, timeout=self.config.io_timeout)
            await asyncio.wait_for(client.connect(), self.config.io_timeout)
            _, caps = await self._control(client, 1)
            if len(caps) != 8 or caps[:2] != b"\x01\x01" or caps[5] & 3 != 3:
                raise ReceiverError("Firmware does not support the v1 prepare/commit session contract")
            if not caps[5] & 4:
                raise ReceiverError("Firmware has no acquisition source adapter; CM-006 integration is required before audio")
            for opcode, value in ((3, AXES[self.config.axis]), (4, self.config.range_g), (5, POLICIES[self.config.policy])):
                await self._control(client, opcode, value)
            await asyncio.wait_for(client.start_notify(STATUS_UUID, status_callback), self.config.io_timeout)
            status_subscribed = True
            deadline = loop.time() + self.config.prepare_timeout
            while True:
                result, payload = await self._control(client, 6, allow_busy=True)
                if not result:
                    break
                if loop.time() >= deadline:
                    raise ReceiverError("Source preparation timed out; inspect firmware diagnostics")
                await asyncio.sleep(0.05)
            if len(payload) != 64:
                raise ProtocolError("START must return a full status snapshot")
            session = decode_status(payload)
            expected_desc = AXES[self.config.axis] | (4 if self.config.range_g == 4 else 0)
            if session.state != 1 or session.descriptor != expected_desc or session.policy != POLICIES[self.config.policy]:
                raise ReceiverError("Prepared session does not match requested source configuration")
            valid_rate(session.rate_millihz)
            self.status = session
            self.last_valid_packet = time.monotonic()
            worker = asyncio.create_task(consume())
            await asyncio.wait_for(client.start_notify(AUDIO_UUID, audio_callback), self.config.io_timeout)
            audio_subscribed = True
            _, payload = await self._control(client, 9, session.session_id)
            self.status = decode_status(payload)
            if self.status.state != 2 or self.status.session_id != session.session_id:
                raise ReceiverError("Firmware did not commit the prepared stream")
            started = loop.time()
            next_report = started
            while True:
                if worker.done():
                    await worker
                if disconnected.is_set() or not client.is_connected:
                    raise ReceiverError("BLE connection lost; reconnect to start a fresh audio session")
                if callback_failure:
                    raise ReceiverError(callback_failure)
                if self.config.duration is not None and loop.time() - started >= self.config.duration:
                    break
                if time.monotonic() - self.last_valid_packet > self.config.idle_timeout:
                    raise ReceiverError("No valid audio within the idle timeout; check source and BLE capacity")
                if loop.time() >= next_report:
                    observed = decode_status(await asyncio.wait_for(client.read_gatt_char(STATUS_UUID), self.config.io_timeout))
                    if (observed.session_id, observed.rate_millihz, observed.descriptor, observed.state) != (
                        session.session_id, session.rate_millihz, session.descriptor, 2):
                        raise ReceiverError("Firmware stopped or changed the active session; reconnect required")
                    self.status = observed
                    if report:
                        report(self.diagnostics(sink))
                    next_report = loop.time() + 1
                await asyncio.sleep(0.02)
        except (ReceiverError, ProtocolError, ValueError):
            primary_failure = True
            raise
        except asyncio.CancelledError:
            primary_failure = True
            raise
        except Exception as exc:
            primary_failure = True
            raise ReceiverError(f"BLE/audio session failed: {exc}. Check the adapter, firmware and output device.") from exc
        finally:
            active = False
            ingress.close()
            if worker is not None:
                worker.cancel()
                try:
                    await worker
                except asyncio.CancelledError:
                    pass
                except Exception as exc:
                    worker_failure = exc
                    self.cleanup_errors.append(f"worker: {exc}")
            if client is not None:
                if client.is_connected:
                    try:
                        _, payload = await self._control(client, 7)
                        self.status = decode_status(payload)
                    except Exception as exc:
                        self.cleanup_errors.append(f"stop: {exc}")
                    for uuid, subscribed in ((AUDIO_UUID, audio_subscribed), (STATUS_UUID, status_subscribed)):
                        if subscribed:
                            try:
                                await asyncio.wait_for(client.stop_notify(uuid), self.config.io_timeout)
                            except Exception as exc:
                                self.cleanup_errors.append(f"unsubscribe: {exc}")
                try:
                    await asyncio.wait_for(client.disconnect(), self.config.io_timeout)
                except Exception as exc:
                    self.cleanup_errors.append(f"disconnect: {exc}")
            self._running = False
        if worker_failure is not None and not primary_failure:
            raise ReceiverError(f"Audio worker failed during shutdown: {worker_failure}") from worker_failure
        return self.diagnostics(sink)
