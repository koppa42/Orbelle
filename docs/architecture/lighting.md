# 光照与地影分析

以 STK Lighting Times 的 Sunlight / Penumbra / Umbra 设计组织输出，首版支持地球遮挡太阳的时段与光照比例。桌面、C++ 和 Python 通过共享 `Analysis::report()` 获取同一结果。

## 模型

- 地球球面半径 6378137 m，太阳球面半径 695700000 m；均匀太阳圆盘，几何太阳位置。
- 输入为地心 GCRF 卫星状态及显式注入的地心太阳位置，长度采用 m、时刻采用 TAI 整数纳秒。SPICE J2000 在牛顿地心模型中按 ICRF 对齐处理，与现有第三体/SRP 契约一致；不声称完整相对论 GCRS 转换。
- 复用 SRP 的视圆盘相交几何和有符号接触面。太阳无遮挡为 Sunlight，部分遮挡（包括环食）为 Penumbra，太阳全遮挡为 Umbra。状态由接触几何判断，避免接触处比例舍入为 0/1 导致错误分类。
- 此分析独立于 SRP 开关、Cr、面积质量比及力模型中的遮挡设置。当前仅含地球球面，不含月球、其他天体、地球椭球、地形、大气折射、太阳光行时及像差。

## 时段与精度

`lighting_intervals` 细化外接触和本影接触的零点，再构造不重叠且覆盖整个查询区间的时段。默认极值辅助搜索、最大检查间隔 60 s、边界容差 1 ms；固定网格也可选，但可能遗漏网格内的短事件。极值辅助可检出单个采样格内的擦边阴影；检查间隔仍必须分辨约束的独立波瓣，不能保证任意多峰函数的所有事件。边界容差控制根细化，不等于模型相对真实轨道的物理误差。最短可分辨时段受容差和检查间隔共同限制。

查询边界处的时段标记 clipped：它们表示分析范围截断，不能解释为接触事件。单时刻查询没有时段、统计为零；`lighting_state` 则返回该时刻的一条状态。多卫星按项目顺序分别输出，汇总分母为卫星数乘查询持续时间，不做不同卫星事件的时间并集。

单遮挡体圆盘面积使用稳定圆缺公式，极短弧采用级数展开，避免擦边处大数相减。固定样本的比例误差由独立 80 位参考约束；不将该数值误差解释为真实轨道预测精度。

`lighting_state` 使用精确采样端点，输出比例基础单位 `1`，工程显示 `%`。连线仅是样本之间的视觉连接，不能用其代替事件根搜索。短半影可能不会出现在较粗的比例采样中。

## 资源、失败与 API

查询固定使用 `ReportQuery.lighting_ephemeris`（默认 `de440s`）。已准备快照必须含此 Provider 及匹配的数据锁，覆盖完整查询区间；失败和取消不发布部分结果。SGP4 的 TEME 状态通过明确注入的 EOP 转换后参与几何计算。几何不合法、资源缺失/不匹配或越界均报错。每次报告最多 200000 行，单对象事件搜索最多 2000000 次几何求值。

`ReportResult.lighting_statistics` 仅时段报告存在。三个状态分别提供段数、累计时长、最长时长、时间占比；修改显示模板不改变统计。来源保留项目输入哈希、数据锁、太阳星历选择及算法标识。项目 v44 / RPTS5 保存查询设置，不保存计算表。

Python 示例见 [lighting_analysis.py](../../examples/python/lighting_analysis.py)，数值参考和误差预算见[验证协议](../validation/lighting_protocol.md)。STK 设计来源：[Lighting Times](https://help.agi.com/stkEngineOnUNIX/Subsystems/dataProviders/Content/html/dataProviders/Lighting_Times~Vehicle.htm)、[Lighting 设置](https://help.agi.com/stk/content/stk/vehSat_eclipseBodiesTab.htm)。本机没有 STK，验证采用 Orekit，不宣称已与 STK 导出结果数值一致。
