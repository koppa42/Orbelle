# 轨道事件分析

轨道事件报告使用已准备的卫星连续状态，求近地点、远地点、升交点和降交点时刻。桌面、C++ 与 Python 调用同一 Core / Runtime 实现，查询不读取显示轨迹、不下载数据。

## 定义与范围

| 事件 | 条件 | 方向 |
| --- | --- | --- |
| 近地点 | 地心径向速率 r·v / ‖r‖ = 0 | 由负变正 |
| 远地点 | 同上 | 由正变负 |
| 升交点 | GCRF z = 0 | 由负变正 |
| 降交点 | GCRF z = 0 | 由正变负 |

所有状态必须同历元、地心、GCRF，单位 SI；TEME 状态经已有 EOP/参考系 Provider 完整转换。交点参考面固定为 GCRF 赤道面，查询中的 Cartesian frame 草稿不改变它。距离输出为地心距离，未将其称为地形高度或椭球高。事件位置与速率来自细化后时刻的同一状态。

STK 的 [Pass Data](https://help.agi.com/stk/SubSystems/dataProviders/Content/htmlStyles/Satellite/Reports/Pass_Data.htm) 使用径向速率过零定义近远地点；[Ascending Node](https://help.agi.com/stkEngineOnUNIX/SubSystems/dataProviders/Content/htmlStyles/Satellite/Reports/Ascending_Node.htm) 使用所选参考系 z 分量过零，并排除近赤道情况。本版将四类事件放在一张表中，不生成圈号或 Pass 区间，也不提供 STK 的全部 pass-break 选项。没有 STK 实机数据，不声称 STK 数值等价。

事件候选状态的瞬时偏心率 e ≤ 1e-8 时不定义近远地点，|sin(i)| ≤ 1e-8 时不定义交点。统计记录查询评估过程中遇到近圆／近赤道状态的对象数，桌面显示说明；这两个阈值是本项目的显式约定，不推测 STK 的内部阈值。近圆轨道的方位和近赤道轨道的交点不通过任意角度约定伪造事件。径向轨迹、非有限状态及无效 GM 明确拒绝。

首版支持现有连续轨道 Session，包括二体、J2/J4、SGP4 和 HPOP；HPOP 固定星历模式使用科学计算插值。Mission 序列明确拒绝，因为跨脉冲的跳变不能作为连续根处理。后续可按滑行段分开搜索。没有任意事件表达式、任意平面穿越、圈号、轨道周期或通用事件编辑器。

## 搜索和结果

`ReportKind::orbital_events` / `ReportKind.ORBITAL_EVENTS` 只接受卫星。`step_nanoseconds` 是最大检查间隔，默认 60 s；`event_tolerance_nanoseconds` 默认 1 ms，是已发现根的数值细化容差，不是轨道模型精度。默认极值辅助搜索在每段内搜索函数的峰谷再细化符号变化；固定网格模式只检查节点。每段须分辨独立波瓣，快速变化时减半检查间隔核对收敛。

全程使用整数 TAI 纳秒；不在请求区间外取状态。孤立且精确落在端点的零点只计一次，区间内部切触和零平台不作为过零事件。单时刻查询返回零事件。不同类型同时发生时保留两行，并按类型排序；每颗卫星的结果按时间升序，不将不同卫星合并成同一个事件。

最多 200000 个检查节点、2000000 次状态评估和 200000 条报告行。每次新评估检查取消，进度单调；失败和取消不发布部分结果。事件结果固定输入、数据身份和算法 ID，模板格式调整复用源表和统计，不重新传播。

报告字段为 `object.id`、`object.name`、`time.epoch`、`event.kind`、`motion.radius`、`motion.speed`。事件字符串稳定为 `Periapsis`、`Apoapsis`、`Ascending node`、`Descending node`；桌面本地化。`orbital_event_statistics.counts` 按同一顺序保存数量，并包含 `object_count`、`near_circular_objects`、`near_equatorial_objects`。

## 界面与 API

“分析 → 报告”或卫星对象菜单中选择“轨道事件 · 近远地点 / 交点”。四张摘要卡显示分类数量，事件表通过颜色区分类别；宽布局并排显示设置与结果，窄布局沿用设置／结果切换及两列摘要卡。双击任意事件行定位场景时刻；支持模板、逐列单位、精确时间提示和 CSV。离散事件不连接成采样曲线，PNG 曲线导出不适用于此表。

```python
events = analysis.report(of.ReportKind.ORBITAL_EVENTS, objects=satellite,
                         step=u.s(60), boundary_tolerance=u.s(.001))
print(events.orbital_event_statistics.counts)
events.export_csv("events.csv")
```

完整示例：[orbital_events.py](../../examples/python/orbital_events.py)。C++ 使用相同 `ReportDefinition` 和 `Analysis::report`，低层求根入口为 `core::find_orbital_events`。当前项目格式 v46 / RPTS7 保存报告枚举 12，保留现有搜索字段；只读写当前格式，不迁移旧版。验证见[轨道事件协议](../validation/orbital_events_protocol.md)。
