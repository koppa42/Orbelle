"""A cold portable launch must cancel its own modal setup before exiting."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'package_windows_launch', ROOT / 'tools/packaging/package_windows.py')
windows = importlib.util.module_from_spec(spec)
spec.loader.exec_module(windows)


class NormalLaunchTests(unittest.TestCase):
    def run_launch(self, setup_count=0, workbench_visible=True, native_caption=False):
        process = Mock(pid=123, returncode=None)
        process.poll.side_effect = lambda: process.returncode
        process.terminate.side_effect = lambda: setattr(process, 'returncode', -15)
        suffix = ' - 星弦 Orbelle' if native_caption else ''
        title = '星弦 Orbelle · 准备基础数据' + suffix
        inventory = {1: (123, '星弦 Orbelle  |  Untitled' + suffix),
                     2: (999, title), 3: (999, '星弦 Orbelle  |  Other project')}
        for index in range(setup_count):
            inventory[10 + index] = (123, title)

        def enumerate_windows(callback, context):
            for handle in inventory:
                callback(handle, context)

        def window_pid(handle, output):
            output._obj.value = inventory[handle][0]

        def window_title(handle, output, length):
            output.value = inventory[handle][1]

        def close(handle, message, word, long):
            self.assertEqual((message, word, long), (0x0010, 0, 0))
            self.assertEqual(inventory[handle][0], process.pid)
            # The main window cannot close while its modal preparation is open.
            if handle == (10 if setup_count else 1):
                process.returncode = 0
            return True

        def wait(timeout):
            if process.returncode is None:
                raise subprocess.TimeoutExpired('Orbelle.exe', timeout)
            return process.returncode

        process.wait.side_effect = wait
        user32 = SimpleNamespace(
            EnumWindows=Mock(side_effect=enumerate_windows),
            GetWindowThreadProcessId=Mock(side_effect=window_pid),
            GetWindowTextW=Mock(side_effect=window_title),
            IsWindowVisible=Mock(side_effect=lambda handle: workbench_visible or handle != 1),
            PostMessageW=Mock(side_effect=close))
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(windows.ctypes, 'WinDLL', return_value=user32, create=True), \
             patch.object(windows.ctypes, 'WINFUNCTYPE',
                          side_effect=lambda *args: lambda callback: callback, create=True), \
             patch.object(windows.subprocess, 'Popen', return_value=process), \
             patch.object(windows.time, 'sleep'):
            root = Path(directory)
            if setup_count > 1:
                with self.assertRaisesRegex(RuntimeError, 'uniquely identify initial data'):
                    windows.verify_normal_launch(root / 'Orbelle.exe', root, {}, root)
                user32.PostMessageW.assert_not_called()
                process.terminate.assert_called_once()
                return
            result = windows.verify_normal_launch(root / 'Orbelle.exe', root, {}, root)
        process.terminate.assert_not_called()
        self.assertEqual(result['exit_code'], 0)
        self.assertEqual(result['initial_data_setup'], bool(setup_count))
        self.assertEqual(result['window_title'], inventory[1][1] if workbench_visible else None)
        self.assertEqual(result['closed_window_title'],
                         title if setup_count else inventory[1][1])
        user32.PostMessageW.assert_called_once_with(10 if setup_count else 1, 0x0010, 0, 0)

    def test_fresh_data_cancels_own_setup_and_exits(self):
        self.run_launch(setup_count=1)

    def test_fresh_data_setup_can_open_before_the_workbench_is_visible(self):
        for native_caption in (False, True):
            with self.subTest(native_caption=native_caption):
                self.run_launch(setup_count=1, workbench_visible=False,
                                native_caption=native_caption)

    def test_ready_data_closes_workbench_and_ignores_other_process_setup(self):
        self.run_launch()

    def test_ambiguous_setup_is_rejected_without_closing_any_window(self):
        self.run_launch(setup_count=2)


if __name__ == '__main__':
    unittest.main()
