# OrbitForge

航天任务分析与仿真平台。当前是已批准目录方案的 **P0 工程骨架**，首个产品版本以 P2 轨道与 Access 分析为边界。

## 当前可运行内容

- 独立构建的 C++20 Core，以及 Results、Application、Runtime、Visualization 模块。
- 一个地心 GCRF 平面内的圆轨道解析案例，C++、独立 Python 包和 Qt 窗口共用 Runtime 入口。
- 可选 ERFA 2.0.1 Provider：显式 UTC↔TAI、TAI→TT 两部分 JD，以及含观测天极偏移的 GCRF↔ITRF 位置和速度变换；C++ 与 Python 共用接口。
- 本地 IERS 20 C04 快照导入、SHA256 校验、跨闰秒插值及严格覆盖范围；附带一周历史验证数据。
- 可选 CSPICE N0067 Provider：显式本地 LSK/SPK 快照、TAI↔TDB、日月几何状态和批量查询；内核与错误状态统一串行管理。
- CMake 安装/导出的 C++ SDK、pybind11/scikit-build-core wheel 构建入口。
- GoogleTest/CTest 核心与集成测试、Python API 测试、安装后的 SDK 消费端案例。
- Linux x64 无界面 Release、ASan/UBSan 和断网环境中的 SDK / Python wheel 验证入口；已验证 Ubuntu 24.04、GCC 13.3、Python 3.12。
- 可选 Qt/VSG/Rocky 双视窗工作台：共享设备与场景、独立相机、ITRF 显示和时间播放；Windows 三档缩放、窗口重建及 Vulkan 验证通过。
- 可选固定万星基准场景：两视图、100 条轨迹、每视图 200 标签、缓存跳转与选中反馈；尚无正式渲染性能测试结果。
- 可选无界面计算基准：批量状态生成、完整参考系变换、CSPICE 单次/批量查询与上下文切换，保存逐轮计时及数值检查。

基础 Qt 窗口显示示例状态表；`desktop-graphics` 双视窗路径已通过 Windows 单星集成检查，结果见[图形验证记录](docs/validation/p0_graphics.md)。时间/参考系通过 Orekit 独立天文级数交叉校验，星历通过 jplephem 读取原始 DE440s 的独立实现对照。尚无日内 EOP 潮汐改正、TEME、通用二体/J2/SGP4、事件/Access、HDF5 结果存储、异步分析任务和安装包。P0 的全部退出条件尚未完成。

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
- [图形集成验收协议](docs/validation/p0_graphics_protocol.md)
- [图形集成验证记录](docs/validation/p0_graphics.md)
- [万星双视图场景预检](docs/validation/p0_graphics_10k.md)
- [双视图呈现计时接入与测量限制](docs/validation/p0_graphics_timing.md)
- [双视图 GPU 与进程资源度量](docs/validation/p0_graphics_resources.md)
- [呈现反馈原始记录与结束排空](docs/validation/p0_presentation_feedback.md)
- [呈现阶段组合受控对照](docs/validation/p0_output_stage_probe.md)
- [Linux 无界面构建与交付验证](docs/validation/p0_linux_headless.md)
- [独立计算吞吐量协议](docs/validation/p0_compute_protocol.md)
- [CSPICE 验证记录](docs/validation/p0_spice.md)
- [EOP 与完整状态变换验证记录](docs/validation/p0_eop_state.md)
- [时间与位置旋转阶段记录](docs/validation/p0_time_frames.md)
- [P0 骨架验收记录](docs/validation/p0_skeleton.md)
- [已批准的工程目录方案](docs/OrbitForge_工程目录方案_v0.1.md)
- [25 项技术路线确认记录](docs/OrbitForge_技术路线_v0.1_评审确认记录.md)
- [原始技术路线草案](docs/OrbitForge_技术路线_v0.1.md)

