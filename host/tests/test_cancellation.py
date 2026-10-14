"""I/O completion/cancellation races independent of platform timer resolution."""
import asyncio
import unittest

from contactmic.receiver import _bounded_await


class BoundedAwaitTests(unittest.IsolatedAsyncioTestCase):
    async def test_completion_and_external_cancel_in_same_turn(self):
        loop = asyncio.get_running_loop()

        async def owner():
            completed = loop.create_future()

            def finish_and_cancel():
                completed.set_result("done")
                task.cancel()

            # The I/O future becomes done before cancellation is delivered. Old
            # wait_for on Python 3.11 incorrectly accepts that completed result.
            loop.call_soon(finish_and_cancel)
            await _bounded_await(completed, 1)
            self.fail("I/O completion swallowed an external cancellation")

        task = asyncio.create_task(owner())
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(task.cancelled())

    async def test_timeout_cancels_inflight_operation_and_runs_cleanup(self):
        cleaned = False

        async def never_completes():
            nonlocal cleaned
            try:
                await asyncio.Future()
            finally:
                cleaned = True

        with self.assertRaises(TimeoutError):
            await _bounded_await(never_completes(), 0.01)
        self.assertTrue(cleaned)
        # An expired I/O deadline does not permanently cancel the owner.
        self.assertEqual(await _bounded_await(asyncio.sleep(0, result=7), 1), 7)

    async def test_immediate_io_stays_in_owner_task(self):
        owner = asyncio.current_task()

        async def immediate():
            self.assertIs(asyncio.current_task(), owner)
            return 42

        self.assertEqual(await _bounded_await(immediate(), 1), 42)


if __name__ == "__main__":
    unittest.main()
