"""Exercise the real standalone Win32 launcher/helper against a harmless native stub."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
import uuid


def managed(folder):
    files = sorted(path for path in folder.rglob('*') if path.is_file()
                   and path.name not in ('orbelle-managed.txt', 'orbelle-managed-files.txt'))
    listing = ''.join(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(folder).as_posix()}\n'
                      for path in files).encode('utf-8')
    (folder / 'orbelle-managed-files.txt').write_bytes(listing)
    (folder / 'orbelle-managed.txt').write_text('1\n' + hashlib.sha256(listing).hexdigest() + '\n',
                                             encoding='ascii', newline='\n')


class NativeUpdater(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='orbelle-updater-')
        self.root = Path(self.temporary.name) / 'Application with spaces'
        self.root.mkdir()
        self.processes = []
        shutil.copy2(OPTIONS.launcher, self.root / 'Orbelle.exe')
        (self.root / 'orbelle-layout.txt').write_bytes(b'1\n')
        (self.root / '.orbelle-running.lock').write_bytes(b'')
        (self.root / 'active-version.txt').write_bytes(b'0.3.1\n')
        self.old = self.payload('0.3.1', (1, 0, 300, 7))
        self.new = self.payload('0.3.2', (1, 25, 350, 12))

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                # Includes this helper's test stub child if an assertion interrupted a run.
                subprocess.run([str(Path(os.environ['SystemRoot']) / 'System32/taskkill.exe'),
                                '/PID', str(process.pid), '/T', '/F'], capture_output=True)
                process.wait(timeout=5)
        self.temporary.cleanup()

    def payload(self, version, configuration):
        folder = self.root / 'versions' / version
        folder.mkdir(parents=True)
        shutil.copy2(OPTIONS.stub, folder / 'OrbelleApp.exe')
        shutil.copy2(OPTIONS.updater, folder / 'OrbelleUpdater.exe')
        shutil.copy2(OPTIONS.launcher, folder / 'OrbelleLauncher.exe')
        (folder / 'stub.cfg').write_text(' '.join(map(str, configuration)), encoding='ascii')
        (folder / 'manifest.json').write_text(json.dumps({'schema': 1, 'version': version}), encoding='utf-8')
        (folder / 'resources').mkdir()
        (folder / 'resources' / 'bundled.dat').write_bytes(b'owned science fixture')
        managed(folder)
        return folder

    def run_process(self, args, **kwargs):
        process = subprocess.Popen(list(map(str, args)), cwd=self.temporary.name, **kwargs)
        self.processes.append(process)
        return process

    def close_diagnostics(self, process):
        user32 = ctypes.WinDLL('user32', use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

        @callback_type
        def close(window, unused):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == process.pid:
                user32.PostMessageW(window, 0x0010, 0, 0)
            return True

        def worker():
            while process.poll() is None:
                user32.EnumWindows(close, 0)
                time.sleep(.02)
        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        return thread

    def update(self, **kwargs):
        parent = self.run_process([self.old / 'OrbelleApp.exe'])
        helper = self.run_process([self.new / 'OrbelleUpdater.exe', '--apply-update', self.root,
                                   '0.3.2', parent.pid], **kwargs)
        self.close_diagnostics(helper)
        return parent, helper

    def test_launcher_forwards_arguments_and_exit_code(self):
        record = Path(self.temporary.name) / 'arguments.txt'
        arguments = ['with spaces', '', 'a"quoted', 'trailing\\', '\\path\\"end', '中文']
        process = self.run_process([self.root / 'Orbelle.exe', '--record', record, *arguments])
        self.assertEqual(process.wait(timeout=5), 7)
        self.assertEqual(record.read_bytes().decode('utf-16-le').split('\n')[:-1], arguments)

    def test_success_waits_for_parent_publishes_and_cleans_old(self):
        with (self.root / 'Orbelle.exe').open('ab') as launcher:
            launcher.write(b'old-launcher-test-overlay')
        parent, helper = self.update()
        time.sleep(.1)
        self.assertIsNone(parent.poll())
        self.assertEqual((self.root / 'active-version.txt').read_bytes(), b'0.3.1\n')
        self.assertEqual(helper.wait(timeout=10), 12)
        self.assertEqual((self.root / 'active-version.txt').read_bytes(), b'0.3.2\n')
        self.assertFalse(self.old.exists())
        self.assertTrue((self.new / 'OrbelleApp.exe').exists())
        self.assertEqual((self.root / 'Orbelle.exe').read_bytes(),
                         (self.new / 'OrbelleLauncher.exe').read_bytes())
        self.assertFalse(list(self.root.glob('Orbelle.pending-*')))

    def test_locked_launcher_preserves_old_program_and_removes_temporary_copy(self):
        self.check_publication_failure('Orbelle.exe')

    def test_locked_active_pointer_keeps_new_launcher_compatible_with_old_program(self):
        self.check_publication_failure('active-version.txt')
        launcher = self.run_process([self.root / 'Orbelle.exe'])
        self.assertEqual(launcher.wait(timeout=5), 7)

    def check_publication_failure(self, filename):
        with (self.root / 'Orbelle.exe').open('ab') as launcher:
            launcher.write(b'old-launcher-test-overlay')
        original_launcher = (self.root / 'Orbelle.exe').read_bytes()
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                       ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                       wintypes.HANDLE]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel32.CreateFileW(str(self.root / filename), 0x80000000, 1, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            _, helper = self.update()
            self.assertEqual(helper.wait(timeout=10), 7)
            self.assertEqual((self.root / 'active-version.txt').read_bytes(), b'0.3.1\n')
            self.assertTrue((self.old / 'OrbelleApp.exe').exists())
            self.assertFalse(list(self.root.glob('Orbelle.pending-*')))
            expected_launcher = original_launcher if filename == 'Orbelle.exe' else (
                self.new / 'OrbelleLauncher.exe').read_bytes()
            self.assertEqual((self.root / 'Orbelle.exe').read_bytes(), expected_launcher)
        finally:
            kernel32.CloseHandle(handle)

    def test_crash_before_ready_preserves_active_and_reopens_old(self):
        (self.new / 'stub.cfg').write_text('0 0 40 88', encoding='ascii')
        parent, helper = self.update()
        self.assertEqual(helper.wait(timeout=10), 7)
        self.assertEqual((self.root / 'active-version.txt').read_bytes(), b'0.3.1\n')
        self.assertTrue(self.old.exists())

    def test_unknown_file_and_empty_directory_preserve_entire_version(self):
        for user_content in ('notes.orbelle', 'empty-user-folder'):
            with self.subTest(content=user_content):
                path = self.old / user_content
                path.mkdir() if 'folder' in user_content else path.write_bytes(b'user project')
        _, helper = self.update()
        self.assertEqual(helper.wait(timeout=10), 12)
        self.assertTrue((self.old / 'notes.orbelle').exists())
        self.assertTrue((self.old / 'resources' / 'bundled.dat').exists())

    def test_data_home_inside_old_version_is_preserved(self):
        env = dict(os.environ, ORBELLE_DATA_HOME=str(self.old / 'resources'))
        _, helper = self.update(env=env)
        self.assertEqual(helper.wait(timeout=10), 12)
        self.assertTrue((self.old / 'resources' / 'bundled.dat').exists())

    def test_invalid_version_and_wrong_parent_leave_pointer_unchanged(self):
        for version, pid in (('../outside', os.getpid()), ('0.3.2', os.getpid())):
            helper = self.run_process([self.new / 'OrbelleUpdater.exe', '--apply-update', self.root, version, pid])
            self.close_diagnostics(helper)
            self.assertEqual(helper.wait(timeout=5), 1)
            self.assertEqual((self.root / 'active-version.txt').read_bytes(), b'0.3.1\n')

    def test_second_launcher_does_not_start_another_app(self):
        first = self.run_process([self.root / 'Orbelle.exe'])
        time.sleep(.05)
        second = self.run_process([self.root / 'Orbelle.exe'])
        self.close_diagnostics(second)
        self.assertEqual(second.wait(timeout=5), 1)
        self.assertEqual(first.wait(timeout=5), 7)

    def test_modified_managed_file_prevents_cleanup(self):
        (self.old / 'resources' / 'bundled.dat').write_bytes(b'locally changed')
        _, helper = self.update()
        self.assertEqual(helper.wait(timeout=10), 12)
        self.assertTrue((self.old / 'OrbelleApp.exe').exists())

    def test_cancelled_parent_handshake_never_installs_on_later_exit(self):
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
        kernel32.CreateEventW.restype = wintypes.HANDLE
        kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel32.SetEvent.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        name = 'Local\\Orbelle.UpdateParent.test.' + uuid.uuid4().hex
        ready = kernel32.CreateEventW(None, True, False, name)
        cancel = kernel32.CreateEventW(None, True, False, name + '.cancel')
        self.assertTrue(ready and cancel)
        try:
            parent = self.run_process([self.old / 'OrbelleApp.exe'])
            helper = self.run_process([self.new / 'OrbelleUpdater.exe', '--apply-update', self.root,
                                       '0.3.2', parent.pid, '--parent-ready-event', name])
            self.close_diagnostics(helper)
            self.assertEqual(kernel32.WaitForSingleObject(ready, 3000), 0)
            self.assertIsNone(parent.poll())
            kernel32.SetEvent(cancel)
            self.assertEqual(helper.wait(timeout=5), 0)
            parent.wait(timeout=5)
            self.assertEqual((self.root / 'active-version.txt').read_bytes(), b'0.3.1\n')
            self.assertTrue(self.old.exists())
        finally:
            kernel32.CloseHandle(ready)
            kernel32.CloseHandle(cancel)

    def test_helper_keeps_run_lock_until_new_desktop_exits(self):
        (self.new / 'stub.cfg').write_text('1 25 1300 12', encoding='ascii')
        _, helper = self.update()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                if (self.root / 'active-version.txt').read_bytes() == b'0.3.2\n':
                    break
            except PermissionError:
                pass  # ReplaceFile briefly excludes incompatible opens of the destination.
            time.sleep(.02)
        self.assertIsNone(helper.poll())
        second = self.run_process([self.root / 'Orbelle.exe'])
        self.close_diagnostics(second)
        self.assertEqual(second.wait(timeout=5), 1)
        self.assertEqual(helper.wait(timeout=5), 12)

    def test_hardlinked_old_file_is_never_deleted(self):
        external = Path(self.temporary.name) / 'shared.dat'
        os.link(self.old / 'resources/bundled.dat', external)
        _, helper = self.update()
        self.assertEqual(helper.wait(timeout=10), 12)
        self.assertEqual(external.read_bytes(), b'owned science fixture')
        self.assertTrue((self.old / 'OrbelleApp.exe').exists())

    def test_reparse_directory_is_rejected_without_touching_target(self):
        target = Path(self.temporary.name) / 'external'
        target.mkdir()
        (target / 'user.txt').write_bytes(b'outside installation')
        link = self.old / 'foreign'
        try:
            os.symlink(target, link, target_is_directory=True)
        except OSError as error:
            command = [str(Path(os.environ['SystemRoot']) / 'System32/cmd.exe'),
                       '/c', 'mklink', '/J', str(link), str(target)]
            created = subprocess.run(command, capture_output=True)
            if created.returncode:
                self.skipTest(f'This account cannot create symbolic links or junctions: {error}')
        _, helper = self.update()
        self.assertEqual(helper.wait(timeout=10), 12)
        self.assertEqual((target / 'user.txt').read_bytes(), b'outside installation')
        self.assertTrue((self.old / 'OrbelleApp.exe').exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--updater', type=Path, required=True)
    parser.add_argument('--stub', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
