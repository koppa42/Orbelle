# P0 骨架的边界与类型契约

本记录描述已经落地的最小契约，不代表已完成全部 P0 功能。

## 依赖

```text
Core <- Results <- Application <- Runtime
          |
          +-- Visualization

C++ example / Python binding -> Runtime
Qt desktop -> Runtime + Visualization + Qt Widgets
```

箭头在此表示右侧模块依赖左侧模块。Core 不链接上层、Qt、Python、VSG、Rocky；Application 使用注入的 `IStateProvider`。具体示例 Provider 由 Runtime 装配。

Results 目前只有拥有自身存储的只读 `StateSeries` 和输入来源元信息。Session 当前为只读、同步采样接口。异步 Job、配置编辑、撤销/重做、结果版本发布和存储后端接口在对应阶段实现，不能从当前字段推定这些能力已经存在。

Visualization 将结果采样点转换为独立显示快照，并提供校验历元、原点、参考系后的显式位置旋转；不包含插值、LOD、帧调度或 GPU 实现。发布后按 const 使用。新增的可选 RenderVsg / GeoRocky 适配层及图形工作台正在进行 P0 集成验证，边界和生命周期见[图形集成约定](graphics.md)。

## 时间

`Epoch` 保存 int64 TAI 纳秒，零点明确为 **2000-01-01 12:00:00 TAI**。这个零点不是 J2000 TT；可选 ERFA Provider 已增加结构化 UTC↔TAI 和 TAI→TT，数据范围及误差语义见[时间与参考系契约](time_frames.md)。当前没有 UTC 字符串解析。

时间差先拆分整秒与纳秒余数，再求差，可避免 int64 相减溢出，也可保留远离零点的相邻纳秒差。它是 P0 类型选择，未来如需超出 int64 纳秒范围，应通过显式 API/Schema 变更处理，不隐式改变时间含义。

## 状态和单位

`CartesianState` 必须同时携带 Epoch、OriginId 和 FrameId，位置字段命名为 `position_m`，速度为 `velocity_mps`。目前用于 Earth 原点、GCRF 和 ITRF。可选 ERFA Provider 已提供含 `R_dot*r` 的 GCRF↔ITRF 完整位置/速度转换，可使用 IERS Provider 从本地 C04 快照取得 EOP 值和导数；详细模型及范围见[时间与参考系契约](time_frames.md)。

星历使用单独的 `EphemerisState`，携带目标/观测者 NAIF 编号、`SPICE_J2000` 轴标签及 `TDB` 速度时间尺度。位置和速度仍为 SI 单位，但不能直接作为上述地心 GCRF 状态使用。TAI↔TDB 的浮点边界、快照归属和并发约定见 [CSPICE 接口](spice.md)。

圆轨道配置使用 `Length` 与 `GravitationalParameter` 区分物理量，采用 SI 单位。Python 使用相同状态定义；浮点历元或 UTC 字符串不能被自动当成 TAI 纳秒。

## 圆轨道案例

`CircularTwoBody` 是 P0 用于贯通接口的解析案例：地心 GCRF XY 平面，初始位置为 +X，速度为 +Y，半径和中心引力参数恒定。运行时示例采用 r=7,000,000 m、mu=3.986004418e14 m³/s²，从零历元每隔 60 s 查询一次，共 6 点。

通过圆周解计算状态，不建立数值积分器，也没有实现通用 Kepler 根数输入。这不是 P1 完整 Two-body/J2/SGP4 能力。解析案例无需外部数据，并未借此绕过未来 EOP、星历及时间转换所需的数据校验。

## API 稳定性

这是 0.1.0 骨架，尚未承诺公共 API 或 ABI 稳定。已安装 C++ 包暂采用精确版本匹配；正式 SDK 兼容策略在实际发布前确定。

