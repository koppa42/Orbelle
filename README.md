# OrbitForge

航天任务分析与仿真平台。当前已进入 **P1 轨道工作台开发**，P0 的全部验收项仍待完成；首个产品版本以 P2 轨道与 Access 分析为边界。

## 当前可运行内容

- 独立构建的 C++20 Core，以及 Results、Application、Runtime、Visualization 模块。
- 一个地心 GCRF 平面内的圆轨道解析案例，C++、独立 Python 包和 Qt 窗口共用 Runtime 入口。
- 可选 ERFA 2.0.1 Provider：显式 UTC↔TAI、TAI→TT 两部分 JD，以及含观测天极偏移的 GCRF↔ITRF 位置和速度变换；C++ 与 Python 共用接口。
- 本地 IERS 20 C04 快照导入、SHA256 校验、跨闰秒插值及严格覆盖范围；附带一周历史验证数据。
- 可选 CSPICE N0067 Provider：显式本地 LSK/SPK 快照、TAI↔TDB、日月几何状态和批量查询；内核与错误状态统一串行管理。
- 通用二体传播：复用 CSPICE `prop2b_c`，接受显式地心 GCRF 初态与 μ，支持椭圆、抛物线、双曲线；C++、Python、桌面共用 Runtime 工厂。
- 一阶 J2 长期项解析传播：独立的平均根数类型，CSPICE 求解椭圆；支持长期项设置，速度包含进动导数。卫星属性支持二体/J2 切换及 GCRF/ITRF 初态输入，μ 统一读取中心天体。
- 经典瞬时根数与 Cartesian 转换：CSPICE `conics_c` / `oscltx_c`；输入 a、e、i、Ω、ω、ν，公开 API 使用米和弧度，桌面使用 km 和度。
- Scenario 场景：先建立统一分析时段，再插入对象；不可变场景版本保存多个 Session，拒绝重复 ID 和过期修订。
- [地面站](docs/architecture/ground_stations.md)：按 WGS84 经纬度与椭球高插入、编辑，二维/三维显示与选择、复制/删除，项目保存及 Python/C++ 计算接口；当前项目 v7 兼容读取 v1–v6。
- 场景属性可修改名称和分析时段：保留卫星初始条件与显示设置，统一重建轨迹，当前时刻限制在新范围内；数据变化须确认，覆盖不足、取消或失败保留原场景。C++ / Python 提供相同的设置更新 API。
- 对象重命名、复制和删除：显示名称与稳定 ID 分离，存续计算修订保留；EOP 快照可从桌面导入、校验、持久保存，打开或重算时确认数据变化，见[对象与数据管理](docs/architecture/object_data_management.md)。
- `.orbitforge` 项目文件：版本化分段容器、CRC 校验和原子保存；保存二体/J2 输入、对象显示设置与数据版本引用，支持桌面/Python/C++ 互通、保存/另存为/打开及未保存提示。外部模型和 EOP 快照仍为独立依赖，详见[项目文件格式](docs/architecture/project_files.md)。
- [报告与曲线](docs/architecture/reports.md)：提供摘要、位置速度、地面站坐标、轨道根数、经纬高与轨道速率共七种报告；支持多对象曲线、缩放和时刻定位、CSV/PNG 导出、对象绑定、数据变更确认和项目 v7 模板保存。桌面与 Python 共用 Runtime。
- Scenario 数据锁：保存 EOP 和时间数据的内容 SHA-256、解释配置及依赖清单总哈希；旧项目可升级，已有场景的数据不会随目录更新自动改变，详见[数据引用契约](docs/architecture/scenario_data_lock.md)。
- 统一数据中心：左侧“地球定向参数”下分 finals2000A、C04 两个子项。选中父级可在右侧拖动调整计算优先级，选中子级查看获取时间、覆盖范围及最近检查结果。默认 C04 优先，近期及预测时段回退 finals2000A，实际覆盖优先于外推。三种数据均支持自动获取完整文件，支持手动更新、检查频率、下载进度和失败重试。详情分为概览、更新与计算、文件信息，UTC 更新同步重建 EOP 时间依赖。场景打开或计算时检查数据和策略变化并请求确认，哈希用于溯源，详见[数据中心](docs/architecture/data_center.md)。
- CMake 安装/导出的 C++ SDK、pybind11/scikit-build-core wheel 构建入口。
- GoogleTest/CTest 核心与集成测试、Python API 测试、安装后的 SDK 消费端案例。
- Linux x64 无界面 Release、ASan/UBSan 和断网环境中的 SDK / Python wheel 验证入口；已验证 Ubuntu 24.04、GCC 13.3、Python 3.12。
- 可选 Qt/VSG/Rocky 双视窗工作台：共享设备与场景、独立相机，三维默认 GCRF、可切换 ITRF；地球随时间旋转，支持昼夜光照。
- 桌面场景工作流：新建场景、插入多颗卫星，二维/三维与属性共用可拖动标签页，支持分屏、合并、排序、弹出和放回；属性按组导航，表单宽度受限；离线地球贴图、可选三维模型，默认以点显示。
- 默认一圈、60 s显示步长，按需准备局部轨迹缓存，未缓存时刻的位置由 Session 即时求值；基本分组设置显示范围，二维/三维分别配置显示。
- 二维地面地图：ITRF/WGS84 经纬网、星下点、跨日期变更线轨迹、昼夜分界、缩放平移及浮动停靠，与三维共享时间和卫星选择。
- UTC/TAI/TT 日期与时间输入、弹出日历/时间选择、默认 UTC 今天零点至次日零点；当前 IERS Bulletin A 数据快照随便携包部署并明确标记预测值。
- 分层时间轴：场景总览、可缩放刻度、当前时刻游标与精确跳转；可收起为播放控制条，所选卫星的传播区间按需展开。
- Windows 便携目录与 ZIP 打包入口：收集 DLL、Qt 插件、MSVC 运行库和离线资源，验证无开发环境 PATH、异地工作目录下的启动。
- 可选固定万星基准场景：两视图、100 条轨迹、每视图 200 标签、缓存跳转与选中反馈；尚无正式渲染性能测试结果。
- 可选无界面计算基准：批量状态生成、完整参考系变换、CSPICE 单次/批量查询与上下文切换，保存逐轮计时及数值检查。

基础 Qt 窗口显示示例状态表；`desktop-graphics` 提供场景工作台，操作说明见[场景与属性工作流](docs/architecture/scenario_workbench.md)。时间/参考系通过 Orekit 独立天文级数交叉校验，星历通过 jplephem 读取原始 DE440s 的独立实现对照。尚无日内 EOP 潮汐改正、TEME、SGP4、事件/Access、HDF5 结果存储或通用分析任务系统。便携包为开发预览，P0 的全部退出条件和 P1/P2 尚未完成。

## 快速构建

需要 CMake 3.28+ 和 C++20 编译器。在仓库根目录运行：

```powershell
cmake --preset headless
cmake --build --preset headless
cmake --install out/build/headless --config Release
```

Windows/MSVC 示例程序：`out/build/headless/examples/cpp/Release/orbitforge_example.exe`。单配置生成器的可执行文件位于同级目录，不含 `Release/`。

只构建 Core：

```powershell
cmake --preset core-only
cmake --build --preset core-only
```

## 目录入口

| 目录 | 内容 |
|---|---|
| `modules/` | 可独立构建的 C++ 模块，采用 `include/src/tests` 布局 |
| `apps/desktop/` | Qt Widgets 示例工作台 |
| `bindings/python/` | 原生绑定、Python 包与 API 测试 |
| `examples/` | C++/Python 示例 |
| `tests/` | 跨模块测试和安装后的 SDK 验证 |
| `benchmarks/scenarios/` | 性能验收场景定义 |
| `cmake/`、`tools/ci/` | 构建、安装与验证入口 |
| `docs/` | 已确认方案、开发说明与验证记录 |
| `out/`、`local/` | 被 Git 忽略的构建产物及本机数据 |

C++ SDK 由模块安装生成。Python 构建入口 `pyproject.toml` 放在仓库根目录，以便 wheel 和源码包使用同一份 C++ 源码；Python 实现仍在 `bindings/python/`。

## 开发与设计文档

- [构建、测试与交付验证](docs/development/build.md)
- [模块边界和 P0 类型契约](docs/architecture/p0_contracts.md)
- [时间与参考系契约](docs/architecture/time_frames.md)
- [CSPICE 星历与时间接口](docs/architecture/spice.md)
- [图形集成约定](docs/architecture/graphics.md)
- [场景与属性工作流](docs/architecture/scenario_workbench.md)
- [项目文件格式与桌面/Python 互通](docs/architecture/project_files.md)
- [项目保存与打开验证](docs/validation/project_files_2026_10_10.md)
- [基础报告、STK 对照与后续阶段](docs/architecture/reports.md)
- [报告计算、桌面与便携包验证](docs/validation/reports_2026_10_11.md)
- [报告单对象范围与左右布局验证](docs/validation/report_ui_2026_10_11.md)
- [报告曲线、模板交互与界面打磨验证](docs/validation/report_charts_2026_10_11.md)
- [二体与场景工作台验证记录](docs/validation/p1_two_body_workbench.md)
- [时间轴交互与便携包验证](docs/validation/timeline_2026_10_09.md)
- [工作区、视图与按需缓存验证](docs/validation/workspace_views_2026_10_10.md)
- [工作台界面细化与便携包验证](docs/validation/workbench_ui_2026_10_10.md)
- [工作台视觉统一与高 DPI 检查](docs/validation/ui_refresh_2026_10_10.md)
- [场景名称与分析时段编辑验证](docs/validation/scenario_settings_2026_10_10.md)
- [通用二体验证协议](docs/validation/p1_two_body_protocol.md)
- [图形集成验收协议](docs/validation/p0_graphics_protocol.md)
- [图形集成验证记录](docs/validation/p0_graphics.md)
- [万星双视图场景预检](docs/validation/p0_graphics_10k.md)
- [双视图呈现计时接入与测量限制](docs/validation/p0_graphics_timing.md)
- [双视图 GPU 与进程资源度量](docs/validation/p0_graphics_resources.md)
- [呈现反馈原始记录与结束排空](docs/validation/p0_presentation_feedback.md)
- [呈现阶段组合受控对照](docs/validation/p0_output_stage_probe.md)
- [Linux 无界面构建与交付验证](docs/validation/p0_linux_headless.md)
- [独立计算吞吐量协议](docs/validation/p0_compute_protocol.md)
- [独立计算吞吐量基线](docs/validation/p0_compute.md)
- [CSPICE 验证记录](docs/validation/p0_spice.md)
- [EOP 与完整状态变换验证记录](docs/validation/p0_eop_state.md)
- [时间与位置旋转阶段记录](docs/validation/p0_time_frames.md)
- [P0 骨架验收记录](docs/validation/p0_skeleton.md)
- [已批准的工程目录方案](docs/OrbitForge_工程目录方案_v0.1.md)
- [25 项技术路线确认记录](docs/OrbitForge_技术路线_v0.1_评审确认记录.md)
- [原始技术路线草案](docs/OrbitForge_技术路线_v0.1.md)

