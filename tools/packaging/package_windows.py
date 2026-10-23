"""Build a relocatable Windows desktop folder with runtime DLLs, plugins and offline data."""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

# Reviewed runtime inventories for the pinned dependencies. Do not copy the build
# resource tree or vcpkg share directories wholesale: both contain developer/test
# files and may retain outputs from earlier build configurations. Dependency updates
# that change their runtime data must update this inventory explicitly.
EARTH_RUNTIME_FILES = frozenset("""
earth.vrt
land_ocean_ice_2048.jpg
manifest.json
NOTICE.txt
""".split())
ROCKY_RUNTIME_SHADERS = frozenset("""
rocky.atmo.glsl
rocky.atmo.sky.frag
rocky.atmo.sky.vert
rocky.debug.frag.glsl
rocky.depthoffset.glsl
rocky.icon.frag
rocky.icon.indirect.cull.comp
rocky.icon.indirect.frag
rocky.icon.indirect.vert
rocky.icon.vert
rocky.lighting.glsl
rocky.line.frag
rocky.line.vert
rocky.mesh.frag
rocky.mesh.vert
rocky.point.frag
rocky.point.vert
rocky.projection.glsl
rocky.terrain.frag
rocky.terrain.vert
rocky.viewdependentstate.glsl
shadows.glsl
shadows_hard.glsl
shadows_pcss.glsl
shadows_soft.glsl
""".split())
PROJ_RUNTIME_FILES = frozenset("""
CH
GL27
ITRF2000
ITRF2008
ITRF2014
ITRF2020
deformation_model.schema.json
nad.lst
nad27
nad83
other.extra
proj.db
proj.ini
projjson.schema.json
triangulation.schema.json
world
""".split())
GDAL_RUNTIME_FILES = frozenset("""
bag_template.xml
cubewerx_extra.wkt
default.rsc
ecw_cs.wkt
eedaconf.json
epsg.wkt
esri_StatePlane_extra.wkt
gdal_algorithm.schema.json
gdalg.schema.json
gdalinfo_output.schema.json
gdalmdiminfo_output.schema.json
gdaltileindex.xsd
gdalvrt.xsd
gfs.xsd
gml_registry.xml
gml_registry.xsd
grib2_center.csv
grib2_process.csv
grib2_subcenter.csv
grib2_table_4_2_0_0.csv
grib2_table_4_2_0_1.csv
grib2_table_4_2_0_13.csv
grib2_table_4_2_0_14.csv
grib2_table_4_2_0_15.csv
grib2_table_4_2_0_16.csv
grib2_table_4_2_0_17.csv
grib2_table_4_2_0_18.csv
grib2_table_4_2_0_19.csv
grib2_table_4_2_0_190.csv
grib2_table_4_2_0_191.csv
grib2_table_4_2_0_2.csv
grib2_table_4_2_0_20.csv
grib2_table_4_2_0_21.csv
grib2_table_4_2_0_22.csv
grib2_table_4_2_0_3.csv
grib2_table_4_2_0_4.csv
grib2_table_4_2_0_5.csv
grib2_table_4_2_0_6.csv
grib2_table_4_2_0_7.csv
grib2_table_4_2_1_0.csv
grib2_table_4_2_1_1.csv
grib2_table_4_2_1_2.csv
grib2_table_4_2_10_0.csv
grib2_table_4_2_10_1.csv
grib2_table_4_2_10_191.csv
grib2_table_4_2_10_2.csv
grib2_table_4_2_10_3.csv
grib2_table_4_2_10_4.csv
grib2_table_4_2_2_0.csv
grib2_table_4_2_2_3.csv
grib2_table_4_2_2_4.csv
grib2_table_4_2_2_5.csv
grib2_table_4_2_2_6.csv
grib2_table_4_2_2_7.csv
grib2_table_4_2_20_0.csv
grib2_table_4_2_20_1.csv
grib2_table_4_2_20_2.csv
grib2_table_4_2_20_3.csv
grib2_table_4_2_3_0.csv
grib2_table_4_2_3_1.csv
grib2_table_4_2_3_2.csv
grib2_table_4_2_3_3.csv
grib2_table_4_2_3_4.csv
grib2_table_4_2_3_5.csv
grib2_table_4_2_3_6.csv
grib2_table_4_2_4_0.csv
grib2_table_4_2_4_1.csv
grib2_table_4_2_4_10.csv
grib2_table_4_2_4_2.csv
grib2_table_4_2_4_3.csv
grib2_table_4_2_4_4.csv
grib2_table_4_2_4_5.csv
grib2_table_4_2_4_6.csv
grib2_table_4_2_4_7.csv
grib2_table_4_2_4_8.csv
grib2_table_4_2_4_9.csv
grib2_table_4_2_local_Canada.csv
grib2_table_4_2_local_HPC.csv
grib2_table_4_2_local_index.csv
grib2_table_4_2_local_MRMS.csv
grib2_table_4_2_local_NCEP.csv
grib2_table_4_2_local_NDFD.csv
grib2_table_4_5.csv
grib2_table_versions.csv
gt_datum.csv
gt_ellips.csv
header.dxf
inspire_cp_BasicPropertyUnit.gfs
inspire_cp_CadastralBoundary.gfs
inspire_cp_CadastralParcel.gfs
inspire_cp_CadastralZoning.gfs
jpfgdgml_AdmArea.gfs
jpfgdgml_AdmBdry.gfs
jpfgdgml_AdmPt.gfs
jpfgdgml_BldA.gfs
jpfgdgml_BldL.gfs
jpfgdgml_Cntr.gfs
jpfgdgml_CommBdry.gfs
jpfgdgml_CommPt.gfs
jpfgdgml_Cstline.gfs
jpfgdgml_ElevPt.gfs
jpfgdgml_GCP.gfs
jpfgdgml_LeveeEdge.gfs
jpfgdgml_RailCL.gfs
jpfgdgml_RdArea.gfs
jpfgdgml_RdASL.gfs
jpfgdgml_RdCompt.gfs
jpfgdgml_RdEdg.gfs
jpfgdgml_RdMgtBdry.gfs
jpfgdgml_RdSgmtA.gfs
jpfgdgml_RvrMgtBdry.gfs
jpfgdgml_SBAPt.gfs
jpfgdgml_SBArea.gfs
jpfgdgml_SBBdry.gfs
jpfgdgml_WA.gfs
jpfgdgml_WL.gfs
jpfgdgml_WStrA.gfs
jpfgdgml_WStrL.gfs
leaflet_template.html
MM_m_idofic.csv
netcdf_config.xsd
nitf_spec.xml
nitf_spec.xsd
ogr_fields_override.schema.json
ogrinfo_output.schema.json
ogrvrt.xsd
osmconf.ini
ozi_datum.csv
ozi_ellips.csv
pci_datum.txt
pci_ellips.txt
pdfcomposition.xsd
pds4_template.xml
plscenesconf.json
ruian_vf_ob_v1.gfs
ruian_vf_st_uvoh_v1.gfs
ruian_vf_st_v1.gfs
ruian_vf_v1.gfs
s57agencies.csv
s57attributes.csv
s57expectedinput.csv
s57objectclasses.csv
seed_2d.dgn
seed_3d.dgn
stateplane.csv
template_tiles.mapml
tms_LINZAntarticaMapTileGrid.json
tms_MapML_APSTILE.json
tms_MapML_CBMTILE.json
tms_NZTM2000.json
trailer.dxf
vdv452.xml
vdv452.xsd
vicar.json
""".split())


def copy_runtime_resources(source_resources, prefix, output):
    """Copy only reviewed production data, independently of dirty build outputs."""
    source_resources, prefix, output = map(Path, (source_resources, prefix, output))
    if output.exists():
        raise ValueError('Runtime resource output must be a new directory')
    groups = (
        ('earth', source_resources / 'earth', EARTH_RUNTIME_FILES),
        ('rocky/shaders', prefix / 'share/rocky/shaders', ROCKY_RUNTIME_SHADERS),
        ('proj', prefix / 'share/proj', PROJ_RUNTIME_FILES),
        ('gdal', prefix / 'share/gdal', GDAL_RUNTIME_FILES),
    )
    copies = []
    for relative, source, names in groups:
        for name in sorted(names):
            path = source / name
            if not path.is_file():
                raise RuntimeError(f'Required runtime resource is missing: {path}')
            copies.append((path, output / relative / name))
    output.mkdir(parents=True)
    for source, destination in copies:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)



def find_crt(vc_root, toolset):
    """VS may ship its default redistributable under a different servicing patch."""
    redist = vc_root / 'Redist/MSVC'
    versions = [toolset]
    default = vc_root / 'Auxiliary/Build/Microsoft.VCRedistVersion.default.txt'
    if default.is_file():
        version = default.read_text(encoding='utf-8-sig').strip()
        if re.fullmatch(r'\d+\.\d+\.\d+(?:\.\d+)?', version):
            if tuple(map(int, version.split('.')[:2])) >= tuple(map(int, toolset.split('.')[:2])):
                versions.append(version)
    for version in versions:
        roots = list((redist / version / 'x64').glob('Microsoft.VC*.CRT'))
        if len(roots) == 1:
            return roots[0]
    raise RuntimeError('Cannot locate a compatible app-local MSVC runtime from this Visual Studio installation')


def validate_output_locations(output, reports):
    """Keep local diagnostics outside the distributable directory."""
    output, reports = output.resolve(), reports.resolve()
    if output.is_relative_to(reports) or reports.is_relative_to(output):
        raise ValueError('Package output and local reports must be separate, non-nested directories')


def write_package_manifest(output, reports, executable, dll_sources):
    validate_output_locations(output, reports)
    entries = [{'path': path.relative_to(output).as_posix(), 'bytes': path.stat().st_size,
                'sha256': digest(path)} for path in sorted(output.rglob('*')) if path.is_file()]
    manifest = {'schema': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                'executable_sha256': digest(executable),
                'dll_sources': [{'file': entry['file'], 'sha256': entry['sha256']}
                                for entry in dll_sources], 'files': entries}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    shutil.copy2(output / 'manifest.json', reports / 'package-manifest.json')
    (reports / 'dll-sources.json').write_text(
        json.dumps(dll_sources, indent=2) + '\n', encoding='utf-8')
    return entries


def write_managed_version_manifest(folder, version):
    """Mark an immutable payload for conservative, verified old-version cleanup."""
    names = {'orbelle-managed.txt', 'orbelle-managed-files.txt', 'manifest.json'}
    entries = [{'path': path.relative_to(folder).as_posix(), 'bytes': path.stat().st_size,
                'sha256': digest(path)} for path in sorted(folder.rglob('*'))
               if path.is_file() and path.relative_to(folder).as_posix() not in names]
    manifest = {'schema': 1, 'version': version, 'files': entries}
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    paths = sorted(path for path in folder.rglob('*') if path.is_file()
                   and path.relative_to(folder).as_posix() not in names - {'manifest.json'})
    listing = ''.join(f'{digest(path)}  {path.relative_to(folder).as_posix()}\n' for path in paths)
    (folder / 'orbelle-managed-files.txt').write_bytes(listing.encode('utf-8'))
    (folder / 'orbelle-managed.txt').write_bytes(
        ('1\n' + hashlib.sha256(listing.encode('utf-8')).hexdigest() + '\n').encode('ascii'))


def verify_normal_launch(executable, cwd, environment, reports):
    """Launch with no flags (Explorer-equivalent), and close only our own visible window."""
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL
    windows = []
    setup_windows = []
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

    class ProcessEntry(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
                    ('th32ProcessID', wintypes.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                    ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                    ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG),
                    ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]

    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    def descendants(parent):
        snapshot = kernel32.CreateToolhelp32Snapshot(2, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        links = {}
        try:
            entry = ProcessEntry()
            entry.dwSize = ctypes.sizeof(entry)
            present = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
            while present:
                links[entry.th32ProcessID] = entry.th32ParentProcessID
                present = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            kernel32.CloseHandle(snapshot)
        result = {parent}
        while True:
            found = {child for child, owner in links.items() if owner in result}
            if found <= result:
                return result
            result |= found

    with (reports / 'normal-launch-stdout.log').open('wb') as stdout, (reports / 'normal-launch-stderr.log').open('wb') as stderr:
        process = subprocess.Popen([str(executable)], cwd=cwd, env=environment, stdout=stdout, stderr=stderr)
        owned_pids = {process.pid}

        @callback_type
        def inspect(window, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value in owned_pids and user32.IsWindowVisible(window):
                title = ctypes.create_unicode_buffer(512)
                user32.GetWindowTextW(window, title, len(title))
                # Qt appends applicationDisplayName to native Windows captions.
                caption = title.value.removesuffix(' - 星弦 Orbelle')
                # Identify the completed workbench, not the splash, for a graceful close.
                if caption.startswith('星弦 Orbelle  |  '):
                    windows.append((window, title.value))
                elif caption == '星弦 Orbelle · 准备基础数据':
                    setup_windows.append((window, title.value))
            return True

        try:
            deadline = time.monotonic() + 30
            while not (windows or setup_windows) and time.monotonic() < deadline and process.poll() is None:
                owned_pids = descendants(process.pid)
                user32.EnumWindows(inspect, 0)
                time.sleep(0.1)
            if not (windows or setup_windows):
                raise RuntimeError('Packaged EXE did not open its desktop or initial setup window')
            # Allow the normal frame loop to run, then request a graceful close.
            time.sleep(2)
            if process.poll() is not None:
                raise RuntimeError('Normal desktop exited before its window was closed')
            # Re-enumerate to avoid sending WM_CLOSE to a stale or reparented native handle.
            windows.clear()
            setup_windows.clear()
            owned_pids = descendants(process.pid)
            user32.EnumWindows(inspect, 0)
            if len(windows) > 1 or not (windows or setup_windows):
                raise RuntimeError('Cannot uniquely identify the normal desktop window')
            if len(setup_windows) > 1:
                raise RuntimeError('Cannot uniquely identify initial data preparation')
            # A fresh data directory shows setup before the workbench becomes visible.
            # Closing setup follows Cancel and exit, including while the parent is hidden.
            close_window = setup_windows[0] if setup_windows else windows[0]
            if not user32.PostMessageW(close_window[0], 0x0010, 0, 0):  # This process only.
                raise ctypes.WinError(ctypes.get_last_error())
            if process.wait(timeout=30) != 0:
                raise RuntimeError('Normal desktop launch/close failed')
        finally:
            if process.poll() is None:
                if owned_pids - {process.pid}:
                    # A failed launcher smoke must not orphan its versioned desktop.
                    # The PID is the launcher created above, never an existing app instance.
                    subprocess.run([str(Path(os.environ['SystemRoot']) / 'System32/taskkill.exe'),
                                    '/PID', str(process.pid), '/T', '/F'],
                                   capture_output=True, timeout=15)
                process.terminate()
                process.wait(timeout=10)
    return {'command': [str(executable)], 'window_title': windows[0][1] if windows else None,
            'closed_window_title': close_window[1], 'initial_data_setup': bool(setup_windows),
            'exit_code': process.returncode}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--dependency-prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reports', type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--version', help='Require the desktop build to have this release version')
    parser.add_argument('--zip', action='store_true', help='Also create a ZIP with one top-level application folder')
    args = parser.parse_args()
    if os.name != 'nt':
        raise RuntimeError('Windows packaging requires native PE dependency inspection')
    build = args.build_dir.resolve(strict=True)
    prefix = args.dependency_prefix.resolve(strict=True)
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    version = re.search(r'^CMAKE_PROJECT_VERSION:[^=]+=(.+)$', cache, re.M).group(1).strip()
    if args.version and args.version != version:
        raise ValueError(f'Desktop version {version} differs from requested release {args.version}')
    linker = Path(re.search(r'^CMAKE_LINKER:[^=]+=(.+)$', cache, re.M).group(1))
    dumpbin = linker.with_name('dumpbin.exe').resolve(strict=True)
    vc_root = next(parent for parent in linker.parents if parent.name == 'VC')
    toolset = next(parent.name for parent in linker.parents if parent.parent.name == 'MSVC')
    crt_root = find_crt(vc_root, toolset)
    source = build / 'apps/desktop/Release'
    executable = source / 'orbelle_desktop.exe'
    if not executable.is_file():
        raise RuntimeError('Build the Release desktop before packaging')
    root_output = args.output.resolve()
    output = root_output / 'versions' / version
    reports = args.reports.resolve()
    validate_output_locations(root_output, reports)
    archive = root_output.parent / f'{root_output.name}.zip'
    if args.zip and archive.exists():
        raise RuntimeError(f'Archive already exists: {archive}')
    root_output.mkdir(parents=True, exist_ok=False)
    output.mkdir(parents=True)
    reports.mkdir(parents=True, exist_ok=False)
    native = build / 'apps/desktop/updater/Release'
    for name in ('Orbelle.exe', 'OrbelleUpdater.exe'):
        if not (native / name).is_file():
            raise RuntimeError(f'Build the standalone native launcher/helper before packaging: {name}')
    shutil.copy2(native / 'Orbelle.exe', root_output / 'Orbelle.exe')
    shutil.copy2(native / 'OrbelleUpdater.exe', output / 'OrbelleUpdater.exe')
    shutil.copy2(native / 'Orbelle.exe', output / 'OrbelleLauncher.exe')
    (root_output / 'orbelle-layout.txt').write_bytes(b'1\n')
    (root_output / 'active-version.txt').write_bytes((version + '\n').encode('ascii'))
    (root_output / '.orbelle-running.lock').write_bytes(b'')
    packaged_executable = output / 'OrbelleApp.exe'
    shutil.copy2(executable, packaged_executable)
    copy_runtime_resources(source / 'resources', prefix, output / 'resources')
    plugins = []
    for subdir in ('platforms', 'imageformats', 'tls'):
        target = output / 'plugins' / subdir
        target.mkdir(parents=True)
        files = ([prefix / 'Qt6/plugins/platforms/qwindows.dll'] if subdir == 'platforms'
                 else sorted((prefix / 'Qt6/plugins' / subdir).glob('*.dll')))
        if subdir == 'tls' and not any(path.name == 'qschannelbackend.dll' for path in files):
            raise RuntimeError('The Windows data downloader requires the Qt Schannel TLS backend')
        for path in files:
            shutil.copy2(path, target / path.name)
            plugins.append(target / path.name)
    # Put redistributables beside the EXE before resolving so System32's optional
    # installations cannot mask a missing application-local C++ runtime/loader.
    for path in [*crt_root.glob('*.dll'), prefix / 'bin/vulkan-1.dll']:
        shutil.copy2(path, output / path.name)
    (output / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=plugins\n', encoding='utf-8')
    resolver = Path(__file__).with_name('resolve_windows_runtime.cmake').resolve()
    command = ['cmake', f'-DEXECUTABLE={packaged_executable}',
               '-DPLUGINS=' + ';'.join(map(str, plugins)),
               f'-DSEARCH_DIRECTORIES={output};{prefix / "bin"}', f'-DDUMPBIN={dumpbin}',
               f'-DREPORT={reports / "resolved-dlls.txt"}', '-P', str(resolver)]
    run = subprocess.run(command, capture_output=True)
    (reports / 'dependency-resolution.log').write_bytes(run.stdout + run.stderr)
    run.check_returncode()
    dll_sources = []
    for line in (reports / 'resolved-dlls.txt').read_text(encoding='utf-8').splitlines():
        path = Path(line).resolve(strict=True)
        if not path.is_relative_to(prefix) and not path.is_relative_to(output):
            raise RuntimeError(f'Dependency resolved outside the chosen prefix: {path}')
        target = output / path.name
        if target.exists() and digest(target) != digest(path):
            raise RuntimeError(f'Conflicting runtime DLL: {path.name}')
        if path != target:
            shutil.copy2(path, target)
        dll_sources.append({'file': path.name, 'source': str(path), 'sha256': digest(target)})
    # Preserve dependency notices, including statically linked libraries and data licenses.
    repository = Path(__file__).resolve().parents[2]
    for relative, name in (
        ('modules/providers/sgp4/third_party/LICENSE', 'SGP4.txt'),
        ('modules/core/src/propagation/integrators/rklib-LICENSE.txt', 'rklib-LICENSE.txt'),
        ('modules/providers/spice/third_party/SatelliteToolbox-LICENSE.txt', 'SatelliteToolbox.txt'),
    ):
        (output / 'notices').mkdir(exist_ok=True)
        shutil.copy2(repository / relative, output / 'notices' / name)
    for notice in sorted((prefix / 'share').glob('*/copyright')):
        target = output / 'notices' / notice.parent.name / 'copyright'
        target.parent.mkdir(parents=True)
        shutil.copy2(notice, target)
    (root_output / 'README.txt').write_text(
        f'星弦 Orbelle {version} Windows x64\n\n解压整个文件夹，然后双击 Orbelle.exe。\n'
        '请保留完整目录；根目录 Orbelle.exe 会启动 versions 中的当前版本。\n'
        '先新建场景（Ctrl+N），默认UTC今天零点至次日零点，再插入对象（Ctrl+Shift+I）。\n'
        '数据 → 数据中心：统一管理 C04、finals2000A 和 UTC/闰秒，支持完整文件下载、导入与自动更新。\n'
        '打开或重新计算时检查当前数据；数据变化须确认后继续，项目记录实际使用的数据哈希。\n'
        '计算时段须在数据覆盖范围内，EOP 可在数据中心设置有限外推；UTC/闰秒不外推。\n'
        '日期、时间、UTC/TAI/TT合并输入；点击日历图标展开选择器。\n'
        '数值框支持直接输入和键盘微调，轨道支持经典瞬时根数与位置/速度。\n'
        '传播器可选二体与J2长期摄动，点击“模型…”查看模型信息；显示步长在轨道页设置。\n'
        '初态支持GCRF/ITRF位置速度，轨道根数在GCRF下定义，μ继承中心天体。\n'
        'J2使用平均椭圆；其输入位置速度是辅助开普勒状态，切换模型不执行瞬时/平均转换。\n'
        '也可从启动页明确选择“打开二体示例”。新场景默认不包含卫星。\n'
        '双击场景树对象或右键“属性”打开工作区标签页；可以弹出或放回。\n'
        '基本分组设置轨道和轨迹范围，二维/三维分组设置显示与模型。\n'
        '卫星属性可设置轨迹线型、颜色和粗细；对象右键可生成报告与曲线。\n'
        '默认显示一圈，显示步长60秒；跳转后按需补算，默认卫星为点标记。\n'
        '三维默认GCRF惯性系，可切换ITRF；二维为ITRF/WGS84。\n'
        'Ctrl+Space 播放/暂停；Ctrl+0 适配轨道；Ctrl+L 定位卫星；Ctrl+T 跳转时间。\n\n'
        'Ctrl+J 收起/展开时间轴，保留播放控制、当前时间和浏览范围。\n'
        'Ctrl+3 打开二维地图，与三维共享时间和选择；滚轮缩放、拖动平移、双击复位。\n\n'
        'Run Orbelle.exe from this folder.\n'
        'Versioned application files, DLLs, Qt plugins and offline imagery are included.\n'
        'Keep the folder intact. A Vulkan-capable GPU and its vendor driver are required.\n'
        'No Python, Qt SDK or development PATH is required to launch the desktop.\n'
        f'Earth imagery credit: NASA Goddard Space Flight Center. See versions/{version}/resources/earth/NOTICE.txt.\n'
        'The bundled C++ runtime is from Microsoft Visual Studio, used under its redistributable terms.\n'
        'This is a development preview. See the project documentation for supported models and limitations.\n', encoding='utf-8')
    write_managed_version_manifest(output, version)
    entries = write_package_manifest(root_output, reports, executable, dll_sources)
    launcher = root_output / 'Orbelle.exe'
    if args.verify:
        environment = {key: value for key, value in os.environ.items()
                       if not key.upper().startswith(('QT_', 'QML', 'VSG', 'ROCKY', 'GDAL', 'PROJ', 'VK_'))}
        system_root = Path(os.environ['SystemRoot'])
        environment['PATH'] = os.pathsep.join(map(str, (system_root / 'System32', system_root)))
        environment['ORBELLE_DATA_HOME'] = str(reports / 'user-data')
        # Validation fixtures live outside the delivered runtime. Normal launches ignore them.
        environment['ORBELLE_TEST_RESOURCES'] = str(source / 'resources')
        # Run from an unrelated empty directory to verify relative resource discovery.
        cwd = reports / 'empty-cwd'
        cwd.mkdir()
        smoke = reports / 'portable-smoke'
        command = [str(launcher), '--portable-smoke', str(smoke)]
        run = subprocess.run(command, cwd=cwd, env=environment, capture_output=True, timeout=120)
        (reports / 'portable-stdout.log').write_bytes(run.stdout)
        (reports / 'portable-stderr.log').write_bytes(run.stderr)
        run.check_returncode()
        report = json.loads((smoke / 'report.json').read_text(encoding='utf-8'))
        if report['failure'] or not all(report[key] for key in
                ('shared_device', 'shared_scene', 'independent_cameras', 'all_windows_closed_before_shutdown')):
            raise RuntimeError('Packaged desktop smoke failed')
        normal_launch = verify_normal_launch(launcher, cwd, environment, reports)
        for entry in entries:
            if digest(root_output / entry['path']) != entry['sha256']:
                raise RuntimeError(f"Packaged file changed during launch: {entry['path']}")
        (reports / 'verification.json').write_text(json.dumps({
            'passed': True, 'path': environment['PATH'], 'cwd': str(cwd),
            'vulkan_validation_enabled': report['vulkan_validation_enabled'],
            'scope': 'same-host relocated folder with development environment paths removed; not a clean VM',
            'command': command, 'normal_launch': normal_launch}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.zip:
        with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for path in sorted(root_output.rglob('*')):
                if path.is_file():
                    bundle.write(path, (Path(root_output.name) / path.relative_to(root_output)).as_posix())
        with zipfile.ZipFile(archive) as bundle:
            if bundle.testzip() is not None:
                raise RuntimeError('ZIP integrity check failed')
        (reports / 'archive.json').write_text(json.dumps({'path': str(archive), 'bytes': archive.stat().st_size,
                                                        'sha256': digest(archive)}, indent=2) + '\n', encoding='utf-8')
        from update_bundles import generate_update_bundles
        generate_update_bundles(root_output, version, root_output.parent)
    total = sum(entry['bytes'] for entry in entries)
    print(json.dumps({'output': str(root_output), 'files': len(entries), 'bytes': total,
                      'runtime_dlls': len(list(output.glob('*.dll'))), 'verified': args.verify,
                      'zip': str(archive) if args.zip else None}))


if __name__ == '__main__':
    main()
