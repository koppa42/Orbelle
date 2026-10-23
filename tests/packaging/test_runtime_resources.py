"""Production packages must not inherit test fixtures or build-directory debris."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/packaging"))
import package_windows as packaging
sys.path.pop(0)


class RuntimeResourcesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "build/resources"
        self.prefix = self.root / "dependencies"
        self.output = self.root / "portable/resources"
        self.groups = (
            (self.source / "earth", packaging.EARTH_RUNTIME_FILES),
            (self.prefix / "share/rocky/shaders", packaging.ROCKY_RUNTIME_SHADERS),
            (self.prefix / "share/proj", packaging.PROJ_RUNTIME_FILES),
            (self.prefix / "share/gdal", packaging.GDAL_RUNTIME_FILES),
        )
        for directory, names in self.groups:
            for name in names:
                self.write(directory / name, f"runtime {name}".encode())

    @staticmethod
    def write(path, contents=b"developer or test material"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)

    def copy(self):
        packaging.copy_runtime_resources(self.source, self.prefix, self.output)

    def test_complete_runtime_keeps_imagery_shaders_and_geospatial_data(self):
        self.copy()
        for name in (
            "earth/earth.vrt", "earth/land_ocean_ice_2048.jpg", "earth/NOTICE.txt",
            "rocky/shaders/rocky.terrain.frag", "rocky/shaders/rocky.atmo.glsl",
            "rocky/shaders/shadows_pcss.glsl", "proj/proj.db", "proj/proj.ini",
            "gdal/gdalvrt.xsd", "gdal/grib2_table_4_2_0_0.csv", "gdal/osmconf.ini",
        ):
            with self.subTest(name=name):
                self.assertEqual((self.output / name).read_bytes(),
                                 f"runtime {Path(name).name}".encode())
        self.assertEqual({path.name for path in self.output.iterdir()},
                         {"earth", "rocky", "proj", "gdal"})

    def test_dirty_build_and_dependency_directories_cannot_add_unreviewed_files(self):
        build_debris = (
            "models/satellite_demo.gltf", "eop/eop.c04", "eop-current/finals2000A.data",
            "font/RobotoMono-Regular.ttf", "windows_10k_two_views.json", "fixture.json",
            "orbelle_desktop.pdb", "tests/input.csv", "earth/test.jpg", "earth/README.md",
            "rocky/shaders/rocky.terrain.vert", "rocky/shaders/test.frag",
            "proj/proj.db", "gdal/extra.csv",
        )
        for name in build_debris:
            self.write(self.source / name)
        dependency_debris = (
            "rocky/shaders/test.frag", "rocky/shaders/README.md",
            "proj/proj-config.cmake", "proj/proj-targets-debug.cmake", "proj/test.db",
            "proj/vcpkg_abi_info.txt", "proj/vcpkg.spdx.json", "proj/usage",
            "gdal/GDALConfig.cmake", "gdal/vcpkg-cmake-wrapper.cmake", "gdal/vcpkg.spdx.json",
            "gdal/vcpkg_abi_info.txt", "gdal/usage", "gdal/gdalicon.png", "gdal/GDALLogoColor.svg",
            "gdal/developer.csv", "gdal/grib2_test.csv", "gdal/tests/fixture.xml",
        )
        for name in dependency_debris:
            self.write(self.prefix / "share" / name)
        self.copy()
        files = {path.relative_to(self.output).as_posix()
                 for path in self.output.rglob("*") if path.is_file()}
        self.assertFalse(set(build_debris) - {"rocky/shaders/rocky.terrain.vert", "proj/proj.db"}
                         & files)
        self.assertFalse(set(dependency_debris) & files)
        self.assertEqual((self.output / "rocky/shaders/rocky.terrain.vert").read_bytes(),
                         b"runtime rocky.terrain.vert")
        self.assertEqual((self.output / "proj/proj.db").read_bytes(), b"runtime proj.db")

    def test_missing_required_data_fails_before_creating_partial_resources(self):
        for path in (self.source / "earth/land_ocean_ice_2048.jpg",
                     self.prefix / "share/rocky/shaders/shadows.glsl",
                     self.prefix / "share/proj/proj.db",
                     self.prefix / "share/gdal/gdalvrt.xsd"):
            with self.subTest(path=path):
                original = path.read_bytes()
                path.unlink()
                try:
                    with self.assertRaisesRegex(RuntimeError, "Required runtime resource is missing"):
                        self.copy()
                    self.assertFalse(self.output.exists())
                finally:
                    path.write_bytes(original)

    def test_existing_destination_is_never_merged_with_new_resources(self):
        self.write(self.output / "unowned.txt", b"keep existing content")
        with self.assertRaisesRegex(ValueError, "must be a new directory"):
            self.copy()
        self.assertEqual((self.output / "unowned.txt").read_bytes(), b"keep existing content")
        self.assertEqual(list(self.output.iterdir()), [self.output / "unowned.txt"])


if __name__ == "__main__":
    unittest.main()
