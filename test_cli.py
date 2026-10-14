"""Offline lifecycle tests: no firmware calls, sudo, or module loading."""
import importlib.machinery
import importlib.util
from pathlib import Path
import unittest

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


if __name__ == '__main__':
    unittest.main()
