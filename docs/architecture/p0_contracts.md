# 模块边界与基础类型契约

桌面、C++ SDK 和 Python 包共用 Core、Application 与 Runtime。图形、文件管理和外部计算库由各自适配层提供，公共类型不依赖 Qt 或具体 Provider。

## 依赖

```text
Core <- Results <- Application <- Runtime
          |
          +-- Visualization

C++ example / Python binding -> Runtime
Qt desktop -> Runtime + Visualization + Qt Widgets
```

箭头表示右侧模块依赖左侧模块。Core 不链接上层、Qt、Python、VSG 或 Rocky；Application 使用注入的 `IStateProvider`，具体 Provider 由 Runtime 装配。

Results 提供拥有自身存储的只读 `StateSeries` 和带类型列、单位及来源信息的 `Table`。Session 是只读、同步采样接口；Scenario 通过新值发布修订，Project 保存可编辑、可序列化输入。桌面后台任务负责计算与取消，目前没有通用 Job 系统、撤销栈或 HDF5 存储后端。报告边界见[报告与曲线](reports.md)。

Visualization 将采样结果转换为独立显示快照，并校验历元、原点和参考系后的显式旋转。窗口、GPU 资源、帧调度及按需显示缓存属于桌面和 RenderVsg / GeoRocky 适配层；边界与生命周期见[图形集成约定](graphics.md)。显示插值不回写分析结果。

## 时间

`Epoch` 保存 int64 TAI 纳秒，零点为 **2000-01-01 12:00:00 TAI**，不是 J2000 TT。ERFA Provider 提供结构化 UTC↔TAI 和 TAI→TT；数据范围与错误语义见[时间与参考系契约](time_frames.md)。公共数值 API 不把浮点历元或 UTC 字符串隐式当成 TAI 纳秒，桌面时间控件负责显式解析输入。

时间差先拆分整秒与纳秒余数，再求差，避免 int64 相减溢出并保留相邻纳秒差。超出 int64 纳秒表示范围的扩展必须通过显式 API/Schema 变更处理。

## 状态和单位

`CartesianState` 同时携带 Epoch、OriginId 和 FrameId；位置为 `position_m`，速度为 `velocity_mps`。地心 GCRF↔ITRF 完整状态变换包含 `R_dot*r`，EOP 值及导数来自明确指定的 IERS 数据。Python 使用相同状态定义。

星历使用 `EphemerisState`，携带目标/观测者 NAIF 编号、`SPICE_J2000` 轴标签及 `TDB` 速度时间尺度。其 SI 数值不能隐式作为地心 GCRF 状态使用；时间浮点边界、快照所有权和并发约定见 [CSPICE 接口](spice.md)。

`Length` 与 `GravitationalParameter` 等类型区分物理量，计算 API 采用 SI 单位。桌面可切换显示单位，在调用计算接口前显式转换。

## 解析案例与轨道模型

`CircularTwoBody` 是贯通接口的解析案例：地心 GCRF XY 平面，初始位置为 +X、速度为 +Y，半径与中心引力参数恒定。运行时基础示例采用 r=7,000,000 m、μ=3.986004418e14 m³/s²，从零历元每隔 60 s 查询一次，共 6 点；无需外部数据。

通用二体与一阶 J2 长期项由独立的 Runtime 工厂提供。二体支持 Cartesian / 经典瞬时根数转换，J2 使用初始平均椭圆；两者的输入含义不同，见[场景工作台](scenario_workbench.md)与 [J2 契约](j2.md)。已加入 J4、SGP4/TLE 和 HPOP 数值积分（见[预报器](propagators.md)）；参考系和时间转换仍须校验各自外部数据。地面站与子传感器的几何可见性见[Access 契约](access_sensors.md)。

## API 稳定性

0.2.0 仍为开发预览，尚未承诺公共 API 或 ABI 稳定。已安装 C++ 包采用精确版本匹配。
