# 构建与测试

从仓库根目录执行命令。Windows 提供桌面工作台、SDK 和 Python 包；Linux 提供无界面构建与交付验证入口，可通过本地 Docker 在隔离目录执行。GitHub Actions 自动检查 PR，并在推送版本标签后发布，见 [CI 与版本发布](ci.md)。Linux 桌面图形尚未验证。

## 首次准备

```powershell
git clone https://github.com/koppa42/OrbitForge.git Orbelle
cd Orbelle
```

- 基础构建：Git、CMake 3.28+ 和 C++20 编译器。Windows 需要 Visual Studio 的 C++ 桌面开发组件与 Windows SDK；Linux 可用 GCC / Clang 和 Ninja。
- 天文计算与桌面：先准备可在命令行调用的 vcpkg，再按下方功能安装依赖。仓库固定 registry 基线和 overlay，公共预设不依赖开发机的绝对路径。
- Python 包：Python 3.11+，构建后端版本由 `pyproject.toml` 固定。
- 桌面启用 `BUILD_TESTING` 时也需要 Python 解释器，用于检查文案资源与引用；文案资源编译只依赖已有的 Qt Core，不需要额外安装 Qt Linguist。
- Windows 图形运行：支持 Vulkan 的 GPU / 驱动；开发启动脚本使用 PowerShell 7（`pwsh`）。Linux 桌面尚未验证。

完整场景工作台使用 `desktop-graphics`；`desktop` 仅用于基础 Qt 状态表。无界面基础示例可直接从下一节开始，不需要安装 vcpkg 依赖。

## 提交范围

Git 保留源码、构建配置、测试与复现脚本、必要的测试输入和运行资源，以及当前接口文档和验证方法。构建日志、逐次 JSON/CSV/XML 报告、截图、开发记录、源码副本和发布包放在已忽略的 `out/` 或 `local/`，不复制进 `docs/`，不使用 `git add -f` 绕过忽略规则。提交前检查 `git diff --cached --stat` 和文件列表；数值容限及复现约定见[验证方法](../validation/README.md)。

## 无界面与 Core

```powershell
cmake --preset headless
cmake --build --preset headless
cmake --preset core-only
cmake --build --preset core-only
```

Core 只使用 C++20 标准库，既不查找也不下载 Qt、Python、VSG 或 Rocky。当前库使用静态链接、位置无关代码，SDK 暂要求精确版本匹配；这不是长期 ABI 承诺。

Visual Studio 多配置生成器使用预设中的 Release 配置；Linux 等单配置生成器使用 `CMAKE_BUILD_TYPE=Release`。可通过命令行或被忽略的 `CMakeUserPresets.json` 指定本机生成器/工具链，不将开发机绝对路径写入公共预设。

## GoogleTest 与依赖基线

`vcpkg.json` 固定 registry 基线为 `9b965a116838c6cdcd36bca60d1b81b030c8ab8d`，默认无依赖，按功能启用。各功能所需依赖由 manifest 的 features 和 overlay 指定。

示例中把下载和编译数据都放入仓库的 `out/`：

```powershell
New-Item -ItemType Directory -Force out/dependencies/downloads | Out-Null
$env:VCPKG_DOWNLOADS = (Resolve-Path out/dependencies/downloads).Path
vcpkg install --x-manifest-root=. --x-install-root=out/dependencies/installed --x-buildtrees-root=out/dependencies/buildtrees --x-packages-root=out/dependencies/packages --triplet=x64-windows --x-feature=tests --x-no-default-features
cmake --preset headless-tests "-DCMAKE_PREFIX_PATH=$PWD/out/dependencies/installed/x64-windows"
cmake --build --preset headless-tests
ctest --preset headless-tests
```

也可在依赖安装后使用 `tools/ci/check.ps1 -DependencyPrefix <安装前缀>`。Linux 应选相应 triplet，并使用相同 CMake/CTest 入口。`vcpkg-configuration.json` 通过 `cmake/ports` 接入 ERFA/CSPICE/Rocky/VSG/vsgQt overlay；未增加其他 registry。

当前测试依赖为 GoogleTest 1.17.0#2，可选计算 Provider 为 ERFA 2.0.1、CSPICE N0067（port 67#4）。EOP/SPICE 文件解析与校验使用 nlohmann-json 3.12.0#2、PicoSHA2 1.0.1 两个头文件库。可选 VSG/Rocky 图形组合见下文；Eigen 和结果 HDF5 存储尚未接入。Python 构建依赖在 `pyproject.toml` 精确指定，不通过 vcpkg 另装一套 Python。

## ERFA 时间与参考系

默认 `headless` / `core-only` 不拉取 ERFA。要启用时间与参考系 Provider，在上文的下载目录配置后执行：

```powershell
vcpkg install --x-manifest-root=. --x-install-root=out/dependencies/installed --x-buildtrees-root=out/dependencies/buildtrees --x-packages-root=out/dependencies/packages --triplet=x64-windows --x-feature=tests --x-feature=time-frames --x-no-default-features
cmake --preset time-frames "-DCMAKE_PREFIX_PATH=$PWD/out/dependencies/installed/x64-windows"
cmake --build --preset time-frames
ctest --preset time-frames
```

也可使用 `tools/ci/check.ps1 -Preset time-frames -DependencyPrefix <安装前缀>`。`ORBELLE_WITH_ERFA=ON` 可以与其他模块开关组合，包括桌面构建。当前桌面表格示例尚未使用新接口。

固定 registry 基线中没有 ERFA port，因此仓库提供一个静态库 overlay。它用 CMake 包装未改动的 ERFA 2.0.1 C 源码，以官方压缩包 SHA512 固定内容。port 同时导出 `erfa::erfa`，选择平台标准 C 数学库，安装原始许可证。当前 Windows 验证使用 x64-windows 的动态 MSVC 运行库，ERFA 本身为静态库。

启用 Provider 的配置阶段会检查固定 IERS 原文的 SHA256，校验失败会停止配置。升级仓库内置数据需要同时审核原文、清单、有效期和边界测试，不能只改有效期。Provider 本身只读取显式数据；桌面数据中心另行负责联网下载与自动更新。

ERFA 自带测试可在取得校验匹配的源码后验证：将 `cmake/ports/erfa/CMakeLists.txt` 和 `erfaConfig.cmake.in` 复制到解压根目录，用单独构建目录加 `-DBUILD_TESTING=ON` 配置，再构建并运行 CTest。上游测试入口为 `t_erfa_c` 和 `t_erfa_c_extra`。

## CSPICE 与完整天文基础构建

`astronomy` 预设在时间/参考系之上启用 CSPICE，并编译样例内核截取工具：

```powershell
vcpkg install --x-manifest-root=. --x-install-root=out/dependencies/installed --x-buildtrees-root=out/dependencies/buildtrees --x-packages-root=out/dependencies/packages --triplet=x64-windows --x-feature=tests --x-feature=time-frames --x-feature=spice --x-no-default-features
cmake --preset astronomy "-DCMAKE_PREFIX_PATH=$PWD/out/dependencies/installed/x64-windows"
cmake --build --preset astronomy
ctest --preset astronomy
```

`ORBELLE_WITH_SPICE=ON` 也可单独组合；它的 Provider 只依赖 Core，UTC 日期转换另由 ERFA/IERS 路线提供。`ORBELLE_BUILD_DATA_TOOLS=ON` 要求 SPICE 已启用。默认 `headless` / `core-only` 均不拉取 CSPICE。

CSPICE overlay 沿用固定基线的 67#4 port 和官方包散列，增加静态库约束、位置无关代码与非 Windows 符号隐藏。Python 扩展包含自己的 CSPICE 实例；C++ 消费端对链接到的同一实例须统一使用 Orbelle 封装。不能在其他线程直接修改它的内核池或错误状态。并发及数据约定见 [CSPICE 接口](../architecture/spice.md)。

## Linux 无界面验证入口

`tools/ci/linux.Dockerfile` 固定 Ubuntu 24.04 基础镜像的 digest，以及与清单相同的 vcpkg 源码提交。APT 工具包来自 Ubuntu 仓库，每次运行保存实际安装版本；工具包并未逐项固定到快照仓库。该入口不安装 Qt/VSG/Rocky，不验证 Linux 桌面。

在仓库根目录的 PowerShell 中，可使用 Docker 的 Linux engine 执行：

```powershell
$orbitRoot = (Get-Location).Path
$orbitRun = Join-Path $orbitRoot ('out/validation/linux-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $orbitRun | Out-Null
docker build --file tools/ci/linux.Dockerfile --tag orbelle-linux-validation:ubuntu24.04 tools/ci
docker run --rm --mount "type=bind,source=$orbitRoot,target=/source,readonly" --mount "type=volume,source=orbelle-linux-validation,target=/work" --mount "type=bind,source=$orbitRun,target=/reports" orbelle-linux-validation:ubuntu24.04 python3 /source/tools/ci/check_linux.py --source /source --work /work --reports /reports/build --delivery /reports/delivery
# 仅在上一条命令成功后执行。消费端不挂载源码或构建卷，并禁用网络。
docker run --rm --network none --mount "type=bind,source=$orbitRun/delivery,target=/input,readonly" --mount "type=bind,source=$orbitRun,target=/reports" orbelle-linux-validation:ubuntu24.04 python3 /input/check_linux.py --stage consumer --delivery /input --work /work --reports /reports/consumer
```

构建阶段依次安装 `x64-linux` 计算/测试依赖、运行 Release 和 ASan/UBSan 测试、生成 wheel、导出 SDK/依赖/测试数据。每轮 SDK 安装和 wheel 输出使用全新目录，避免上轮残留掩盖缺文件。wheel 从逐文件校验的源码副本构建，防止构建后端遍历庞大的本机 `out/`。消费端把导出文件移到新的路径，校验 SHA256，再从安装前缀构建 C++ 案例，并在新虚拟环境离线安装 wheel、运行 Python API 测试和五个示例。`commands.json` 保存每一步命令和退出码；只有全部选定步骤成功才生成 `passed.json`。输出目录已有证据时脚本拒绝覆盖。

可用 `--stage dependencies|release|sanitizers|wheel|export|consumer` 单独执行相应阶段；`sanitizers` 会先准备依赖，再执行全量插桩测试。`--stage tests` 执行依赖、完整 Release 和基准 Python；CI 的 `--stage package` 执行依赖、Release、wheel 和导出，sanitizer 由独立的手动工作流执行；默认 `--stage all` 仍保留全量插桩的本地交付检查。复用构建卷时，为每次执行指定新的 `--reports`。`export` 需要先完成 Release 安装和 wheel 构建，并使用新的 `--delivery`。Sanitizer 检查范围是启用插桩的项目代码；vcpkg 中未插桩的第三方库不能据此宣称内部检查通过。

已有 Linux 工具链和依赖时，也可直接运行 `linux-astronomy` / `linux-sanitizers` 的 CMake 构建和测试预设。这两个预设只在 Linux 显示，使用独立构建目录。Linux wheel 当前是本机构建的 `linux_x86_64` 包，此入口不提供 manylinux 兼容认证。

## C++ SDK 消费端

```powershell
cmake --install out/build/headless --config Release
cmake -S tests/packaging/sdk_consumer -B out/build/sdk-consumer "-DCMAKE_PREFIX_PATH=$PWD/out/install/headless"
cmake --build out/build/sdk-consumer --config Release
ctest --test-dir out/build/sdk-consumer -C Release --output-on-failure
```

消费端只使用安装前缀，通过 `find_package(Orbelle CONFIG REQUIRED COMPONENTS Runtime)` 和 `Orbelle::Runtime` 接入，不引用源码目录或构建目录中的头文件。

带 ERFA 的 SDK 需要同时提供 ERFA 安装前缀，由导出的包配置执行 `find_dependency(erfa 2.0.1 EXACT CONFIG)`：

```powershell
cmake --install out/build/time-frames --config Release --component SDK
cmake -S tests/packaging/sdk_consumer -B out/build/sdk-consumer-time-frames "-DCMAKE_PREFIX_PATH=$PWD/out/install/time-frames;$PWD/out/dependencies/installed/x64-windows" -DORBELLE_EXPECT_TIME_FRAMES=ON "-DORBELLE_EOP_MANIFEST=$PWD/out/install/time-frames/share/Orbelle/data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
cmake --build out/build/sdk-consumer-time-frames --config Release
ctest --test-dir out/build/sdk-consumer-time-frames -C Release --output-on-failure
```

这是可供下游链接的 SDK 安装布局，尚未自动把第三方 C++ 开发包合并成一个发行包。

包含 CSPICE 的 SDK 同时查找 `unofficial-cspice`；完整消费端使用安装后的两类样例数据：

```powershell
cmake --install out/build/astronomy --config Release --component SDK
cmake -S tests/packaging/sdk_consumer -B out/build/sdk-consumer-astronomy "-DCMAKE_PREFIX_PATH=$PWD/out/install/astronomy;$PWD/out/dependencies/installed/x64-windows" -DORBELLE_EXPECT_TIME_FRAMES=ON -DORBELLE_EXPECT_SPICE=ON "-DORBELLE_EOP_MANIFEST=$PWD/out/install/astronomy/share/Orbelle/data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json" "-DORBELLE_SPICE_MANIFEST=$PWD/out/install/astronomy/share/Orbelle/data/spice/de440s_2016_leap/manifest.json"
cmake --build out/build/sdk-consumer-astronomy --config Release
ctest --test-dir out/build/sdk-consumer-astronomy -C Release --output-on-failure
```

## 独立 Python 包

标准 wheel 已启用 ERFA/IERS/CSPICE 和 SQLite 数据目录。先安装以下功能依赖，再在专用构建虚拟环境执行 wheel 构建；隔离构建会安装 `pyproject.toml` 指定的构建后端。常规命令：

```powershell
vcpkg install --x-manifest-root=. --x-install-root=out/dependencies/installed --x-buildtrees-root=out/dependencies/buildtrees --x-packages-root=out/dependencies/packages --triplet=x64-windows --x-feature=tests --x-feature=time-frames --x-feature=spice --x-feature=data-catalog --x-no-default-features
$env:CMAKE_PREFIX_PATH = "$PWD/out/dependencies/installed/x64-windows"
python -m pip wheel . --no-deps --wheel-dir out/dist
python -m venv out/venvs/python-smoke
out/venvs/python-smoke/Scripts/python.exe -m pip install --no-index --find-links out/dist orbelle==0.3.0
out/venvs/python-smoke/Scripts/python.exe -m unittest discover -s bindings/python/tests -v
out/venvs/python-smoke/Scripts/python.exe examples/python/circular_orbit.py
out/venvs/python-smoke/Scripts/python.exe examples/python/time_frames.py
out/venvs/python-smoke/Scripts/python.exe examples/python/eop_state.py
out/venvs/python-smoke/Scripts/python.exe examples/python/sun_moon.py
out/venvs/python-smoke/Scripts/python.exe examples/python/propagators.py
out/venvs/python-smoke/Scripts/python.exe tools/validation/compare_examples.py --cpp-example out/build/headless/examples/cpp/Release/orbelle_example.exe
```

若本机已安装精确匹配的构建依赖，可以在 wheel 命令中加 `--no-build-isolation --no-index` 进行离线构建。在未启动开发者终端的 Windows 环境，可显式设置本机已有的生成器，例如 `$env:CMAKE_GENERATOR = 'Visual Studio 18 2026'`。

第三方 C++ 依赖路径通过 `CMAKE_PREFIX_PATH` 环境变量传入；不要用 `cmake.define.CMAKE_PREFIX_PATH` 覆盖 scikit-build-core 为当前 Python 环境设置的搜索路径，否则可能无法找到该环境中已安装的 pybind11。

Python wheel 只安装 `Python` CMake component，不携带 C++ SDK 和桌面文件。绑定使用与 C++ 一致的静态核心库、ERFA 和 CSPICE，携带对应许可证、时间数据原文、EOP/SPICE 历史快照及清单。计算与快照加载释放 GIL；CSPICE 的加载、时间转换、批量查询和错误清理另由同一个 C++ 互斥锁串行执行。释放 GIL 不表示 CSPICE 可以并行计算。

若复用已装有相同版本的测试环境，应对新生成的 wheel 使用 `pip install --no-index --no-deps --force-reinstall <wheel路径>`，否则可能仍使用旧构建。

源码要求 Python 3.11+；CI 和发布流程验证 Windows / Linux 的 Python 3.11–3.15，本地日常开发只需使用 Python 3.13。平台与发布包范围见 [CI 与版本发布](ci.md)。

## EOP 导入与独立参考验证

标准 wheel 现还启用 `ORBELLE_WITH_DATA_CATALOG=ON`，需要 vcpkg 的 `data-catalog` feature（SQLite）。图形桌面的 `graphics` feature 已显式包含 SQLite、Qt Network 和 OpenSSL。已有依赖目录中没有 SQLite 时，应保留此前所需 features 并在 `vcpkg install` 命令追加 `--x-feature=data-catalog`。

普通 `headless-tests` 不启用数据目录，因此保持无 SQLite 依赖；需要在无界面 C++ SDK 中使用目录时，可配置 `-DORBELLE_WITH_ERFA=ON -DORBELLE_WITH_DATA_CATALOG=ON`。安装后的 SDK 通过 `find_dependency(unofficial-sqlite3)` 接入，Windows wheel 携带相应运行库与许可通知。Windows 便携包还包含 Qt TLS 插件；打包验证使用独立 `ORBELLE_DATA_HOME`，不写用户已有数据目录。

准备本地 IERS 20 C04 0h dX/dY 原始文件后，显式截取并生成新目录：

```powershell
python tools/data/import_c04.py --input out/research/iers/eopc04.1962-now --output-dir local/eop/2016-leap-review --start 2016-12-28 --end 2017-01-04 --snapshot-id local-c04-2016-leap --source-url https://hpiers.obspm.fr/iers/eop/eopc04/eopc04.1962-now --retrieved-on 2026-10-09
python -m unittest discover -s tools/data/tests -v
```

把 `--retrieved-on` 改为所用原文件的实际获取日期。最后日期指 UTC 午夜节点，导入工具拒绝覆盖已有目录；其快照随后可从 C++ 或 Python 的 `load_eop_snapshot` 加载。

独立验证工具需要 Java/Javac（已使用版本 21.0.9）以及 `tools/validation/orekit/dependencies.json` 固定的八个 JAR。按清单 URL 获取对应文件到本地 `out/validation/orekit/jars/`；脚本先核验每个 JAR 的 SHA256，再编译和运行 Java 参考程序：

```powershell
out/venvs/python-smoke/Scripts/python.exe tools/validation/compare_orekit_frames.py --jar-dir out/validation/orekit/jars --work-dir out/validation/orekit/comparison --manifest data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json
```

该命令输出机器可读报告、输入 CSV 与参考矩阵 CSV。Orekit 只作为外部校验工具，不是产品依赖；验证过程不联网、不加载 Orekit 的外部 UTC/EOP 数据。方法、模型对齐和容限见[验证协议](../validation/p0_eop_state_protocol.md)。

## 星历样例重建与独立参考验证

附带的 11 KiB SPK 为 Orbelle 从原始 DE440s 截取的历史样例，包含太阳、地球、月球所需四个段。它只能用于清单规定的 2016/2017 验证窗口。

取得样例 README 指定、SHA256 匹配的两个原始文件后，可在新的输出路径重建相同二进制；工具拒绝覆盖现有文件：

```powershell
out/build/astronomy/modules/providers/spice/Release/orbelle_spice_extract.exe out/research/spice/de440s.bsp out/research/spice/naif0012.tls out/validation/reproduced-de440s.bsp
```

独立参考端使用 jplephem 2.24 / NumPy 2.2.6，安装在验证环境，不是产品运行依赖。`tools/validation/spice_reference_dependencies.json` 记录用于独立参考环境的 Python 3.13 / Windows x64 wheel 文件名、版本与 SHA256；下载后先核验再安装。验证工具检查库版本和完整原始 SPK 散列，再读取未经截取的原始 DE440s 进行对照：

```powershell
out/venvs/python-smoke/Scripts/python.exe tools/validation/compare_spice_ephemeris.py --manifest data/samples/spice/de440s_2016_leap/manifest.json --reference-spk out/research/spice/de440s.bsp --work-dir out/validation/spice-jplephem
```

该工具输出 `report.json` 和原始参考状态 `reference_states.json`。输入包括窗口端点、均匀采样、闰秒边界和固定种子的纳秒采样。容限及输入范围见 [CSPICE 验证协议](../validation/p0_spice_protocol.md)。

## Qt 工作台骨架

```powershell
cmake --preset desktop "-DCMAKE_PREFIX_PATH=<Qt安装前缀>"
cmake --build --preset desktop
```

桌面目标需要 Qt 6.8+ Widgets。manifest 的 `desktop` feature 提供 Qt Widgets 依赖；完整图形路径的固定版本由 vcpkg 基线与 overlay 管理，模块边界见[图形集成约定](../architecture/graphics.md)。

程序位置：`out/build/desktop/apps/desktop/Release/orbelle_desktop.exe`。本机运行时需要 Qt DLL 和 platform plugin 可被找到，可在当前进程环境设置 Qt `bin` 与 `plugins` 路径。支持 `-platform offscreen --smoke-test` 自动退出，用于窗口创建和结果填充验证。完整工作台的 Windows 便携打包方式见下文。

## 双视窗图形集成

`desktop-graphics` 启用 Qt Widgets、VSG 1.1.15、vsgQt 0.4.0 和 Rocky 1.1.0，并使用 ERFA/IERS 把示例结果转为 ITRF。该路径处于集成验证阶段；正式验收以[图形协议](../validation/p0_graphics_protocol.md)和运行报告为准。

```powershell
vcpkg install --x-manifest-root=. --x-install-root=out/dependencies/installed --x-buildtrees-root=out/dependencies/buildtrees --x-packages-root=out/dependencies/packages --triplet=x64-windows --x-feature=tests --x-feature=time-frames --x-feature=spice --x-feature=graphics --x-feature=graphics-validation --x-no-default-features
cmake --preset desktop-graphics "-DCMAKE_PREFIX_PATH=$PWD/out/dependencies/installed/x64-windows"
cmake --build --preset desktop-graphics
ctest --preset desktop-graphics
python tools/validation/run_graphics_smoke.py --executable out/build/desktop-graphics/apps/desktop/Release/orbelle_desktop.exe --dependency-prefix out/dependencies/installed/x64-windows --output out/validation/graphics-first-run
```

每次运行选择新的输出目录。脚本需要真实 Windows 桌面和 Vulkan GPU，会启动图形窗口；不是 offscreen 测试。默认分别运行 100%、150%、200% 的 Qt 缩放，启用 Khronos 同步验证，记录窗口重建、实际呈现、共享设备/场景、独立相机以及交换链图像检查，最后测试缺少 shader 和不可用 Vulkan 驱动的明确失败路径。驱动发现覆盖仅作用于失败测试子进程；实际 DPR 还受显示器系统缩放影响。

正常工作台检查还覆盖属性草稿、应用／取消、报告失效、协方差及任务机动的保存重开。完整交互流程的程序内时限为90秒，图形脚本和便携包验证的进程时限为120秒，为失败后的窗口释放和诊断写入留出时间。该时限用于发现挂起，不是交互性能或数值积分速度的验收门槛。

桌面自动化按以下职责维护，避免为同一操作另起一套驱动：

- `apps/desktop/tests/*_test.cpp` 按控件与功能组织 Qt offscreen 用例；共用 `support/` 中的 QApplication、等待条件、属性字段及报告操作工具。现有 GoogleTest 名称与 CTest 过滤方式保持不变，例如 `ctest --preset desktop-graphics -R '^desktop\.'`。`orbelle_quantity_tests` 为兼容现有调用保留目标名，包含数量控件、轨道、传感器、任务和 HPOP 属性测试。
- `apps/desktop/tests/graphics_smoke/` 包含真实工作台的具名场景段。各段保留异步阶段顺序，前段创建的项目、窗口及缓存会被后段验证，因此不能任意跳过或单独执行。`.inc` 在工作台类定义后包含，以访问内部集成状态，避免把测试入口暴露为产品 API。公共操作在 `helpers.inc`，调度器留在工作台中；`report.json` 的 `orbit_smoke_scenario` 定位当前场景，`orbit_smoke_scenarios` 记录已完成场景与耗时。
- `tools/validation/desktop_test_support.py` 共用开发环境、完整日志、等待就绪、超时及子进程清理。smoke 明确启用验证层，计时脚本默认关闭；便携包检查继续使用独立的隔离环境。启动工具回归纳入 `desktop.ValidationTools`，也可运行 `python -m unittest discover -s tools/validation/tests -p "test_desktop_*.py" -v`。

三档 DPI 均继续执行完整 smoke，包括业务检查和真实图形生命周期断言；拆分文件与复用辅助代码不减少测试场景。新增检查应放入对应场景或 Qt 测试文件，避免继续向单个大型测试文件追加无关流程。

构建后 shader、EOP 样例与离线地球影像等资源复制到可执行文件旁的 `resources/`。运行脚本只为子进程配置依赖 DLL、Qt platform plugin 和 validation layer；开发路径不等于独立安装包。工作台默认使用带离线影像的 Rocky 地球。渲染模块与 Qt 工作台的边界、后台编译屏障和退出次序见[集成约定](../architecture/graphics.md)。

日常打开图形工作台可执行 `pwsh tools/dev/run-desktop.ps1`。启动器为子进程设置固定依赖和 PROJ/GDAL 数据路径，不改变调用者的环境；它会等待窗口关闭。使用 `-Executable`、`-DependencyPrefix` 可覆盖开发路径，附加程序参数通过 `-DesktopArguments` 传入。

## Windows 便携包

完成 `desktop-graphics` 的 Release 构建后，使用全新的输出目录打包：

```powershell
python tools/packaging/package_windows.py --build-dir out/build/desktop-graphics --dependency-prefix out/dependencies/installed/x64-windows --output out/releases/Orbelle-0.3.0-win-x64 --reports out/validation/package-win-x64 --verify --zip
```

脚本收集应用程序、运行库、Qt 插件、离线资源及第三方许可，并生成同名 ZIP。`--verify` 在真实 Windows 桌面与 Vulkan GPU 上检查打包结果；运行时使用隔离的数据目录。解压后的入口是 `Orbelle.exe`。输出与报告必须使用互不包含的新目录。公开 `manifest.json` 只包含包内相对路径、文件名和校验值；本机 DLL 来源路径留在 `--reports` 的 `dll-sources.json`。完整参数可用 `python tools/packaging/package_windows.py --help` 查看。

交付脚本回归测试：`python -m unittest discover -s tests/packaging -p "test_*.py" -v`。

## 万星双视图场景预检

复用上述已安装图形依赖，显式打开可选基准目标：

```powershell
cmake --preset graphics-benchmark "-DCMAKE_PREFIX_PATH=$PWD/out/dependencies/installed/x64-windows"
cmake --build --preset graphics-benchmark
ctest --preset graphics-benchmark
python tools/validation/run_graphics_smoke.py --executable out/build/graphics-benchmark/apps/desktop/Release/orbelle_desktop.exe --dependency-prefix out/dependencies/installed/x64-windows --output out/validation/graphics-10k-first-run --scales 1 --benchmark-scenario benchmarks/scenarios/windows_10k_two_views.json
```

该命令是带验证层的固定负载预检，检查 10,000 标记、100 条轨迹、每视图 200 标签、缓存跳转和选中反馈；不输出帧时间达标结论。两个原生交换链必须恰为 1280×720，DPR 无法表示该像素尺寸时明确拒绝，不能用相近分辨率替代。例如 DPR 1.5 时，逻辑尺寸由工具换算并核验实际帧缓冲。正常播放使用 10 倍仿真时间速度，固定相机；smoke 使用确定性历元和合成 Qt 鼠标事件，不能用其回读等待测量交互延迟。

手动检查可使用开发启动器传入两个参数：

```powershell
tools/dev/run-desktop.ps1 -Executable out/build/graphics-benchmark/apps/desktop/Release/orbelle_desktop.exe -DesktopArguments @('--benchmark-scenario', "$PWD/benchmarks/scenarios/windows_10k_two_views.json")
```

冻结负载和采样计划见 `benchmarks/scenarios/windows_10k_two_views.json`。首像素输出的明确呈现 ID 关联、局部时钟校准及 CPU/RAM 采样已接入可选基准；仍有反馈缺失，尚无正式性能结论。计时边界见[图形集成约定](../architecture/graphics.md)。升级 VSG overlay 后须同步重建依赖其 ABI 的 vsgQt、vsgXchange、Rocky 和应用程序。字体来源和许可证在 `data/samples/fonts/roboto_mono_3_001/`，构建时复制到可执行文件旁的 `resources/font/`。

## 独立计算吞吐量

`compute-benchmark` 只启用 Core / Results / Application / Runtime 与 ERFA/IERS/CSPICE，
不查找 Qt、VSG、Rocky。复用 `tests`、`time-frames`、`spice` vcpkg features 的已安装依赖：

```powershell
cmake --preset compute-benchmark "-DCMAKE_PREFIX_PATH=$PWD/out/dependencies/installed/x64-windows"
cmake --build --preset compute-benchmark
ctest --preset compute-benchmark
python tools/validation/run_compute_benchmark.py --executable out/build/compute-benchmark/benchmarks/compute/Release/orbelle_compute_benchmark.exe --profile baseline --output out/reports/compute-first-run
```

每次使用新输出目录。runner 从可执行文件所在的 CMake 构建树保存配置、可用的依赖状态、
二进制及源码散列，记录运行前后输入一致性；`raw.json` 保留所有轮次，`summary.json`
仅汇总有效测量。完整工作量、计时边界和数值门槛见[计算协议](../validation/p0_compute_protocol.md)。
`--profile smoke` 只做少量功能检查，不能替代正式工作量。

Linux 可用相同 preset 加 `-G Ninja` 和 Linux 依赖前缀；单配置生成器的程序路径没有 `Release/`。
已有 Linux 验证环境支持 `python3 tools/ci/check_linux.py --stage compute --source /src --work /work --reports /reports/new-compute-run`，
依次运行 Release 与 ASan/UBSan 的完整 headless 测试和小工作量，不自动开展 Linux 性能测试。
`--stage all` 保持原交付流程；计算基准阶段需显式选择。

## 格式与检查

项目自有 C++ 目标启用较严格警告并默认视警告为错误；第三方目标不继承项目的告警策略。`.clang-format` 和 `.clang-tidy` 记录统一配置。

`ORBELLE_ENABLE_SANITIZERS` 提供 GCC/Clang 地址与未定义行为检查入口。项目目标及其公开头文件消费端继承插桩选项，第三方静态库不自动插桩。Linux 使用 `linux-sanitizers` 预设；Windows/MSVC 不会静默忽略此选项，而是明确报错。

## 构建接口资料

- [pybind11 官方构建说明](https://pybind11.readthedocs.io/en/stable/compiling.html)
- [scikit-build-core 构建与包配置说明](https://scikit-build-core.readthedocs.io/en/latest/configuration/index.html)

当前代码按本机实际使用的精确依赖版本验证；文档站点的最新版本不代表已经升级项目依赖。


## 新增预报器

SGP4 的固定来源 C++ 代码随仓库编译，无额外网络依赖；其解析与传播测试包含在现有 CTest 入口。J4 的辅助椭圆转换使用 CSPICE，TEME 变换与 UTC 解释使用 ERFA，因此完整验证采用 `astronomy` 或 `desktop-graphics` 预设。参见[模型说明](../architecture/propagators.md)。
