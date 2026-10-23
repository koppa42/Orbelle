# 星间可见性与相对运动

按 STK 的主对象/目标对象与 RIC 定义提供两个内置报告。两者显式绑定 **[基准卫星、目标卫星]**；交换对象保持距离及距离变化率不变，但改变 RIC 基准。只接受两颗不同卫星，暂不接受传感器、全部对象或含机动的任务序列。二体、J2、J4、SGP4、HPOP 共用已准备的科学会话；不使用显示轨迹缓存。

| 报告 | 输出 |
| --- | --- |
| 星间相对运动 · RIC | 时刻、距离、距离变化率、惯性相对速率、RIC 径向/沿轨/法向相对位置、可见 0/1；表格与曲线 |
| 星间可见时段 | 起止时刻、持续时间、两端分析边界截断标志、每段最近距离及其时刻 |

两者的 `ReportResult.intersatellite_statistics` 保存整个查询区间的连续最小/最大距离、对应时刻、可见时段和时长统计。全局距离极值包括不可见时刻；每段最近距离只在该可见窗口内搜索。模板删列和单位切换不改变统计或时间轴数据。相对运动报告步长同时控制输出采样和搜索最大检查间隔。采样包含精确起止时刻；单时刻返回一条相对状态，零个正时长可见区间。

## 坐标与速度

同步查询两颗卫星，转换为地心 GCRF；相对位置为目标减基准。RIC 径向是基准位置单位向量，法向是基准位置与惯性速度叉积的单位向量，沿轨是法向叉径向。输出位置投影，不输出旋转 RIC 坐标导数。基准速度为零或归一化角动量不大于 1e-14 时拒绝生成。

距离变化率是相对惯性速度在两者视线上的投影，远离为正。惯性相对速率是两者 GCRF 速度差的模。不同 ID 的卫星允许位置重合，此时距离和 RIC 位置为零，距离变化率为空。J2/J4/SGP4 速度沿用对应传播器状态约定，不承诺等于位置曲线的精确时间导数；连续最近距离直接优化距离，不依赖速度过零。

这些定义参照 [STK RIC Coordinates](https://help.agi.com/stkdevkit/Subsystems/dataProviders/Content/html/dataProviders/RIC_Coordinates.htm) 与 [STK Basic Constraints](https://help.agi.com/stk/Content/stk/constraints-01.htm)。本机没有 STK，尚未进行 STK 同配置运行对照。

## 遮挡与搜索

两颗卫星在同一历元转换到 ITRF，采用 WGS84 椭球（a=6378137 m、1/f=298.257223563）检查连接线段是否进入椭球内部。计算椭球缩放后的线段到原点的最小平方距离减 1，非负即无遮挡。相切计为几何可见，但孤立零时长接触不输出区间；端点在椭球内为不可见。此量是无量纲约束裕量，不是掠过高度。

可见性使用明确确认的 UTC/EOP 与参考系资源，包括二体 GCRF 输入。算法不含地形、折射、大气高度、光行时、天线视场、链路预算或碰撞概率。遮挡数值对照采用 Orekit `InterSatDirectViewDetector` 的零掠过高度设置，详见[验证协议](../validation/intersatellite_protocol.md)。

固定网格与极值辅助搜索沿用 Access 引擎：事件边界二分细化，距离极值分检查区间用黄金分割搜索并比较端点。快速变化必须减半最大检查间隔检查收敛；极值辅助仍要求每个区间能分辨独立的极值，不保证识别任意高频/不连续轨迹。平坦距离曲线没有唯一最近时刻，返回达到数值极小值的一个历元。行/检查节点最多 200000，查询缓存最多 2000000 个历元，可随时取消；失败不发布部分结果。

## 桌面与 API

报告库提供两种入口，左侧分别选择基准与目标卫星。结果页显示最近/最远距离、可见时段数与总时长；可定位全区间最近时刻、每段最近时刻或任意采样时刻。相对状态支持复用结果的字段/单位切换及 CSV/PNG 导出；可见窗口复用任务时间轴。窄分屏保持现有“编辑设置/查看结果”布局，保留对象绑定和结果。删除对象后保留失效 ID，重新生成明确报错。

```python
relative = analysis.report(ob.ReportKind.RELATIVE_MOTION,
    objects=[primary, target], step=u.s(60), boundary_tolerance=u.s(.001))
passes = analysis.report(ob.ReportKind.INTERSATELLITE_ACCESS,
    objects=[primary, target], step=u.s(60))
closest = relative.intersatellite_statistics.minimum_range  # value: m, epoch: TAI
```

C++ 使用同名 `ReportKind::relative_motion` / `intersatellite_access` 与 `Analysis::report()`；Core 另提供 `relative_motion()` 和 `intersatellite_clearance()`。项目 **v46 / RPTS7** 保存有序双卫星绑定和查询参数，不保存计算结果；只读写当前版本。可运行示例见 [intersatellite.py](../../examples/python/intersatellite.py)。
