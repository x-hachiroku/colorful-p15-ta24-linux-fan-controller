"""Offline lifecycle tests: no firmware calls, sudo, or module loading."""
import importlib.machinery
import importlib.util
from pathlib import Path
import unittest
import tempfile
import subprocess
from unittest import mock

loader = importlib.machinery.SourceFileLoader('cc30_cli', str(Path(__file__).with_name('cc30-fan')))
spec = importlib.util.spec_from_loader(loader.name, loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        cli.STOP.clear()

    def test_bounded_session_refreshes_and_restores(self):
        now = [0]
        writes = []
        def wait(seconds):
            now[0] += seconds
            return False
        cli.max_session(45, write=writes.append, clock=lambda: now[0], wait=wait)
        self.assertEqual(writes, ['max', 'max', 'max', 'auto'])
        self.assertEqual(now[0], 45)

    def test_interrupt_restores_auto(self):
        writes = []
        cli.max_session(30, write=writes.append, clock=lambda: 0, wait=lambda seconds: True)
        self.assertEqual(writes, ['max', 'auto'])

    def test_ambiguous_max_failure_still_restores(self):
        writes = []
        def write(mode):
            writes.append(mode)
            if mode == 'max':
                raise OSError('evaluation failed after possible side effect')
        with self.assertRaises(OSError):
            cli.max_session(30, write=write, clock=lambda: 0)
        self.assertEqual(writes, ['max', 'auto'])

    def test_cleanup_failure_is_reported(self):
        def write(mode):
            if mode == 'auto':
                raise OSError('restore failed')
        with self.assertRaises(OSError):
            cli.max_session(30, write=write, clock=lambda: 0, wait=lambda seconds: True)

    def test_hold_max_renews_until_interruption(self):
        writes = []
        waits = []
        def wait(seconds):
            waits.append(seconds)
            return len(waits) == 3
        cli.control_session('max', None, write=writes.append, wait=wait)
        self.assertEqual(writes, ['max', 'max', 'max', 'auto'])
        self.assertEqual(waits, [20, 20, 20])

    def test_fixed_hold_renews_and_restores_on_interrupt(self):
        writes = []
        duties = []
        checks = []
        waits = []
        def wait(seconds):
            waits.append(seconds)
            return len(waits) == 2
        cli.control_session('fixed', None, percent=47, write=writes.append,
                            set_percent=duties.append, check=lambda: checks.append(True), wait=wait)
        self.assertEqual(duties, [47, 47])
        self.assertEqual(writes, ['auto'])
        self.assertEqual(waits, [1, 1])
        self.assertEqual(len(checks), 2)

    def test_temperature_failure_restores_without_renewing(self):
        writes = []
        duties = []
        def check():
            raise OSError('CPU or GPU reached 85°C')
        with self.assertRaises(OSError):
            cli.control_session('fixed', None, percent=25, write=writes.append,
                                set_percent=duties.append, check=check)
        self.assertEqual(duties, [])
        self.assertEqual(writes, ['auto'])

    def test_gpu_temperature_failure_restores_after_first_duty(self):
        writes = []
        duties = []
        checks = [0]
        def check():
            checks[0] += 1
            if checks[0] == 2:
                raise OSError('GPU temperature unavailable')
        with self.assertRaises(OSError):
            cli.control_session('fixed', None, percent=55, write=writes.append,
                                set_percent=duties.append, check=check, wait=lambda seconds: False)
        self.assertEqual(duties, [55])
        self.assertEqual(writes, ['auto'])

    def test_seconds_and_hold_are_mutually_exclusive(self):
        with mock.patch('sys.argv', ['cc30-fan', 'max', '--seconds', '30', '--hold']):
            with mock.patch('sys.stderr'), self.assertRaises(SystemExit) as result:
                cli.main()
        self.assertEqual(result.exception.code, 2)

    def test_unsafe_percent_rejected_before_hardware_access(self):
        for percent in ('0', '24', '101'):
            with self.subTest(percent=percent), mock.patch('sys.argv', ['cc30-fan', 'fixed', percent]):
                with mock.patch('sys.stderr'), self.assertRaises(SystemExit) as result:
                    cli.main()
                self.assertEqual(result.exception.code, 2)

    def test_real_temperature_guard_checks_threshold_and_invalid_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sensor = root / 'hwmon0'
            sensor.mkdir()
            (sensor / 'name').write_text('coretemp')
            (sensor / 'temp1_input').write_text('65000')
            with mock.patch.object(cli, 'Path', return_value=root):
                for gpu_text, fails in [('60\n', False), ('85\n', True), ('N/A\n', True), ('nan\n', True), ('', True)]:
                    with self.subTest(gpu=gpu_text), mock.patch.object(cli.subprocess, 'run',
                            return_value=subprocess.CompletedProcess([], 0, stdout=gpu_text)):
                        if fails:
                            with self.assertRaises(OSError):
                                cli.check_temperatures()
                        else:
                            cli.check_temperatures()
                (sensor / 'temp1_input').write_text('85000')
                with mock.patch.object(cli.subprocess, 'run',
                        return_value=subprocess.CompletedProcess([], 0, stdout='60\n')):
                    with self.assertRaises(OSError):
                        cli.check_temperatures()

    def test_gpu_query_error_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sensor = root / 'hwmon0'
            sensor.mkdir()
            (sensor / 'name').write_text('coretemp')
            (sensor / 'temp1_input').write_text('65000')
            with mock.patch.object(cli, 'Path', return_value=root), mock.patch.object(
                    cli.subprocess, 'run', side_effect=subprocess.TimeoutExpired('nvidia-smi', 2)):
                with self.assertRaises(OSError):
                    cli.check_temperatures()


if __name__ == '__main__':
    unittest.main()
