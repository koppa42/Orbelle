"""Regression checks for distributable metadata and isolated test inputs."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def load_tool(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


windows = load_tool('package_windows', 'tools/packaging/package_windows.py')
linux = load_tool('check_linux', 'tools/ci/check_linux.py')
sdk = load_tool('package_sdk', 'tools/packaging/package_sdk.py')
python_dependencies = load_tool('export_python_dependencies', 'tools/ci/export_python_dependencies.py')


class WindowsManifestTests(unittest.TestCase):
    def test_default_redist_can_have_a_different_servicing_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            default = root / 'Auxiliary/Build/Microsoft.VCRedistVersion.default.txt'
            default.parent.mkdir(parents=True)
            default.write_text('14.44.35211\n')
            crt = root / 'Redist/MSVC/14.44.35211/x64/Microsoft.VC143.CRT'
            crt.mkdir(parents=True)
            self.assertEqual(windows.find_crt(root, '14.44.35207'), crt)
            with self.assertRaises(RuntimeError):
                windows.find_crt(root, '14.51.36231')

    def test_distributed_manifest_omits_build_host_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, reports = root / 'package', root / 'reports'
            output.mkdir()
            reports.mkdir()
            executable = output / 'OrbitForge.exe'
            executable.write_bytes(b'fixture executable')
            library = output / 'runtime.dll'
            library.write_bytes(b'fixture runtime')
            source = str(root / 'private-build-host' / 'dependencies' / 'runtime.dll')
            dlls = [{'file': library.name, 'source': source, 'sha256': windows.digest(library)}]
            windows.write_package_manifest(output, reports, executable, dlls)

            # A relocated package must be independently verifiable, with no host paths.
            relocated = root / 'relocated'
            shutil.copytree(output, relocated)
            shutil.rmtree(output)
            manifest_text = (relocated / 'manifest.json').read_text(encoding='utf-8')
            manifest = json.loads(manifest_text)
            self.assertNotIn('private-build-host', manifest_text)
            self.assertNotIn('source', manifest['dll_sources'][0])
            self.assertEqual(manifest['executable_sha256'], windows.digest(relocated / 'OrbitForge.exe'))
            self.assertEqual({entry['path'] for entry in manifest['files']}, {'OrbitForge.exe', 'runtime.dll'})
            for entry in manifest['files']:
                path = relocated / entry['path']
                self.assertFalse(Path(entry['path']).is_absolute())
                self.assertEqual(windows.digest(path), entry['sha256'])
                self.assertEqual(path.stat().st_size, entry['bytes'])
            self.assertEqual(json.loads((reports / 'dll-sources.json').read_text()), dlls)
            self.assertFalse((relocated / 'dll-sources.json').exists())

    def test_reports_cannot_be_packaged_by_nesting_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / 'package'
            for reports in (output, output / 'reports', root):
                with self.subTest(reports=reports), self.assertRaises(ValueError):
                    windows.validate_output_locations(output, reports)
            windows.validate_output_locations(output, root / 'reports')


class LinuxDeliveryTests(unittest.TestCase):
    def test_export_keeps_current_project_fixture_after_source_removal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source'
            # Use the fixed current-format input and its actual Python consumer.
            for relative in ('tests/packaging/sdk_consumer', 'tests/reference/time_frames',
                             'tests/integration/fixtures', 'bindings/python/tests', 'examples/python'):
                shutil.copytree(ROOT / relative, source / relative,
                                ignore=shutil.ignore_patterns('__pycache__'))
            for relative in ('tools/ci/check_linux.py', 'tools/ci/summarize_timings.py', 'tools/validation/compare_examples.py',
                             'tools/packaging/package_sdk.py'):
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)
            expected = {path.name: path.read_bytes()
                        for path in (source / 'tests/integration/fixtures').glob('*.orbitforge')}
            self.assertEqual(set(expected), {'project-current.orbitforge'})
            args = argparse.Namespace(source=source, work=root / 'work',
                                      reports=root / 'reports', delivery=root / 'delivery', jobs=1)
            check = linux.Check(args)
            check.source_hashes = check.fingerprint()
            (check.reports / 'source-sha256.json').write_text(json.dumps(check.source_hashes))
            check.prefix.mkdir(parents=True)
            (check.prefix / 'dependency.txt').write_text('fixture dependency')
            (check.work / 'artifacts').mkdir()
            for name in ('release', 'wheel'):
                artifact = check.work / name
                artifact.mkdir()
                (artifact / 'placeholder').write_text('fixture artifact')
                check.save_artifact(name, artifact)
            example = check.work / 'build/release/examples/cpp/orbitforge_example'
            example.parent.mkdir(parents=True)
            example.write_bytes(b'fixture example')
            check.export()
            relocated = root / 'consumer'
            shutil.move(check.delivery, relocated)
            shutil.rmtree(source)

            # This is the same location used by the exported test_project_format.py.
            consumer = relocated / 'bindings/python/tests/test_project_format.py'
            fixture_dir = consumer.parents[3] / 'tests/integration/fixtures'
            self.assertTrue(consumer.is_file())
            for name, contents in expected.items():
                self.assertEqual((fixture_dir / name).read_bytes(), contents)
            entries = json.loads((relocated / 'manifest.json').read_text())
            self.assertIn('tests/integration/fixtures/project-current.orbitforge',
                          {entry['path'] for entry in entries})
            for entry in entries:
                path = relocated / entry['path']
                self.assertEqual(path.stat().st_size, entry['size'])
                self.assertEqual(linux.sha256(path), entry['sha256'])


class SdkDependencyTests(unittest.TestCase):
    def test_sdk_copies_only_required_ports_including_their_licenses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / 'installed/x64-windows'
            info = root / 'installed/vcpkg/info'
            info.mkdir(parents=True)
            for port in ('erfa', 'cspice', 'sqlite3', 'nlohmann-json', 'picosha2', 'qtbase'):
                relative = Path('share') / port / 'copyright'
                path = prefix / relative
                path.parent.mkdir(parents=True)
                path.write_text(port)
                (info / f'{port}_1.0_x64-windows.list').write_text(
                    'x64-windows/\n' + (Path('x64-windows') / relative).as_posix() + '\n')
            sdk.copy_dependencies(prefix, root / 'delivery')
            self.assertFalse((root / 'delivery/share/qtbase').exists())
            for port in ('erfa', 'cspice', 'sqlite3'):
                self.assertEqual((root / f'delivery/share/{port}/copyright').read_text(), port)
            # Matrix jobs receive third-party dependencies, never an OrbitForge SDK or wheel.
            (prefix / 'orbitforge.whl').write_bytes(b'not a dependency')
            archive = root / 'python-dependencies.tar.gz'
            python_dependencies.export(prefix, archive)
            with tarfile.open(archive) as bundle:
                files = {entry.name for entry in bundle.getmembers() if entry.isfile()}
                self.assertEqual(files, {
                    f'dependencies/share/{port}/copyright'
                    for port in ('erfa', 'cspice', 'sqlite3', 'nlohmann-json', 'picosha2')
                })
            original = archive.read_bytes()
            with self.assertRaises(FileExistsError):
                python_dependencies.export(prefix, archive)
            self.assertEqual(archive.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
