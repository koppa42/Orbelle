"""Regression checks for distributable metadata and isolated test inputs."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
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


class WindowsManifestTests(unittest.TestCase):
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
    def test_export_keeps_project_compatibility_fixtures_after_source_removal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source'
            # Use the actual compatibility files and their actual Python consumer.
            for relative in ('tests/packaging/sdk_consumer', 'tests/reference/time_frames',
                             'tests/integration/fixtures', 'bindings/python/tests', 'examples/python'):
                shutil.copytree(ROOT / relative, source / relative,
                                ignore=shutil.ignore_patterns('__pycache__'))
            for relative in ('tools/ci/check_linux.py', 'tools/validation/compare_examples.py'):
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)
            expected = {path.name: path.read_bytes()
                        for path in (source / 'tests/integration/fixtures').glob('*.orbitforge')}
            self.assertIn('project-v1.orbitforge', expected)
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

            # This is the same location used by the exported test_project.py.
            consumer = relocated / 'bindings/python/tests/test_project.py'
            fixture_dir = consumer.parents[3] / 'tests/integration/fixtures'
            self.assertTrue(consumer.is_file())
            for name, contents in expected.items():
                self.assertEqual((fixture_dir / name).read_bytes(), contents)
            entries = json.loads((relocated / 'manifest.json').read_text())
            self.assertIn('tests/integration/fixtures/project-v1.orbitforge',
                          {entry['path'] for entry in entries})
            for entry in entries:
                path = relocated / entry['path']
                self.assertEqual(path.stat().st_size, entry['size'])
                self.assertEqual(linux.sha256(path), entry['sha256'])


if __name__ == '__main__':
    unittest.main()
