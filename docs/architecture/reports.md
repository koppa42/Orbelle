# 基础报告

第一阶段实现于 2026-10-11。参考 STK 的模板、数据提供器与对象绑定方式，报告计算使用 OrbitForge 的 Runtime。桌面和 Python 共用同一个生成、格式化和 CSV 导出实现。

## 使用

打开场景后选择“分析 → 报告”。左侧选择内置模板或已经保存的报告，右侧分为配置、结果、来源三个页签。报告使用现有工作区标签，可分屏或弹出。

对象右键菜单也可打开报告并自动绑定该对象。窄分屏使用顶部模板选择器，表单换行并滚动，底部生成/保存按钮保持可见。“另存为新配置”复制当前绑定和设置，创建独立 ID；内置模板保持不变。结果表支持多选行、右键复制或 Ctrl+C，复制内容带列标题。屏幕数值用 12 位有效数字便于阅读，悬停、复制和 CSV 保留完整精度；屏幕时间省略无意义的尾随零。

| 模板 | 输出 |
| --- | --- |
| 场景摘要 | 名称、起止时间、时间系统、对象数量、中心天体 |
| 对象摘要 | 稳定 ID、显示名称、类型、传播/固定站模型 |
| 位置与速度 | 对象、历元、GCRF/ITRF、XYZ、Vxyz |
| 地面站坐标 | WGS84 经度、大地纬度、椭球高、ITRF XYZ |

“所有适用对象”包含以后插入的对象；关闭后可绑定具体对象 ID。重命名不改变绑定，删除对象后保留配置并在生成时报告缺失，避免无提示地产生不完整报告。

位置与速度可跟随场景范围或使用场景以内的自定义区间。步长是整数纳秒，包含起点和终点；不整除时额外保留精确终点，最后一段较短。允许单时刻查询。每个对象的行连续排列，顺序采用项目的卫星、地面站顺序。

“保存配置”把定义加入当前项目并标记未保存；再保存项目后写入磁盘。项目 v6 保留配置，不保存数据表。旧 v1–v5 项目可直接打开。取消生成或生成失败保留上次结果；切换配置也不会默默替换已有结果。结果页显示所对应的报告名称，场景输入改变后提示重新生成。双击含历元的行可定位场景时间。

## 计算与数据一致性

Application 提供 `ReportDefinition`，Results 提供 `Table`（列 ID、名称、基础单位和有类型单元格），Runtime 提供 `ReportContext`、`generate_report` 和 `export_report_csv`。报告直接由输入重建所选对象的 Session 并采样，不读取显示轨迹 `StateTrack`，显示步长与地图插值不影响结果。卫星和地面站的跨参考系计算使用完整状态变换，包含地球旋转引起的速度。

桌面每次生成前检查当前数据是否覆盖完整场景，以及数据身份/外推策略是否与已确认场景一致。变化时沿用场景的数据确认流程：取消则不生成；同意后先重建场景，成功后自动继续报告。一次报告捕获项目、时间/EOP Provider 的共享不可变版本，下载或后续场景编辑不改变运行中的输入。覆盖不足或 UTC 过期不会静默外推；有限 EOP 外推仍由数据中心控制。

结果记录输入 SHA-256、场景数据锁、UTC 数据 SHA-256 和 `builtin-reports-v1` 算法标识。输入哈希使用规范项目编码，排除报告配置、播放时间、显示步长和纯图形设置，保留物理输入、对象名称和数据身份。该哈希用于识别变化，不是结果缓存键或来源签名。

后台任务每批最多 512 个样本，支持进度与取消；桌面表格按需格式化。第一阶段单次最多 **200000 行**，生成前检查上限；超限应增大步长、缩短区间或减少对象。结果仍驻留内存，尚未实现磁盘分页和无限量导出。

## 单位与 CSV

内部数值始终使用 m、m/s、rad 和 TAI 整数纳秒；工程格式使用 km、km/s、度。UTC 文本保留闰秒，TAI/TT 不带 UTC 的 `Z` 标记，列头明确时间系统。数值格式化采用经典区域设置与 17 位有效数字；时间保留 9 位小数。

CSV 使用 UTF-8 BOM、逗号分隔、双引号转义和 CRLF 行结束符，支持中文、逗号、引号和名称内换行。先写入同目录临时文件，成功后原子发布；默认不覆盖，桌面覆盖须通过文件选择器确认。取消/发布前失败保留旧文件。CSV 只包含表格，不插入哈希前言；本次溯源信息可在来源页和 Python 结果对象中读取，暂不自动导出元数据附属文件。

## Python

```python
from pathlib import Path
import orbitforge as of

project = of.Project("Mission", of.Epoch(0), of.Epoch(120_000_000_000))
orbit = of.OrbitInput()
project.add_satellite(orbit)

report = of.ReportDefinition()
report.id = "satellite-state"
report.name = "轨道位置"
report.kind = of.ReportKind.CARTESIAN_STATE
report.frame = of.FrameId.GCRF
report.step_nanoseconds = 30_000_000_000
report.time_scale = of.ReportTimeScale.UTC

print(of.estimate_report_rows(project, report))
result = of.generate_report(project, report)
# 原始数值为 SI；输出格式由 report.units 控制。
print(result.table.columns[4].unit, result.table.rows[0][4])
result.export_csv(Path("state.csv"))
project.reports = [report]
project.save("mission.orbitforge")
```

ITRF/GCRF 转换须显式提供 `eop_manifest`，并将对应 `data_info` 绑定到 `project.earth_orientation`；如果项目使用外部 UTC 文件，传入匹配的 `time_file`。运行时拒绝与项目记录不一致的数据，不自动联网或代替用户确认。默认使用内置 UTC 数据。输入在释放 Python GIL 前复制，生成和导出期间释放 GIL；Python 结果暴露只读数据及配置副本。`Project.reports` 与其他项目列表一样，修改列表后须整体赋回。

## 后续阶段

1. 可选字段、可复用模板、LLA/根数和曲线图；显示单位调整复用结果，不重复采样。
2. Access、事件区间及汇总；事件查找独立于表格采样。明确区分采样最大值与连续区间极值。
3. 文档排版与 HTML/PDF，元数据随导出保存，批量生成和大结果存储。

第一阶段没有复制 STK 的 `.rpt` 样式格式，也不支持用户公式、任意数据提供器、图表或 PDF。

## 与 STK 的阶段性对照

按 STK 2026 R1 / 13.1 官方帮助对照工作流；本轮没有运行 STK，也没有做双方数值结果的交叉验证。STK 支持从分析菜单或对象进入、选择样式与时间后生成，并提供样式复制等管理操作。[官方工作流](https://help.agi.com/stk/Content/stk/report.htm)

| 对照项 | OrbitForge 本轮状态 | 后续取舍 |
| --- | --- | --- |
| 分析菜单、对象右键入口 | 两处入口，右键自动绑定对象 | 后续支持多对象上下文入口 |
| 内置与用户配置 | 4 个内置模板；项目内保存、复制、删除绑定配置 | 增加独立模板库与导入/导出；当前保存配置不等于任意样式编辑 |
| 时间范围与采样 | 场景/自定义区间、独立步长、精确终点 | 增加离散时刻、区间列表、原始星历节点 |
| 结果交互 | 表格、复制、CSV、时间跳转、工作区分屏/弹出 | 增加查找、列排序与曲线联动 |
| 字段与显示格式 | 固定字段；SI/工程单位；UTC/TAI/TT | 下一步优先做字段选择与顺序、逐维度单位、复用结果的格式调整 |
| 数据变更 | 生成前检查并确认；运行中固定数据；旧结果标记变化 | 保留这一契约，并补外部数据更新提示 |
| 统计与区间 | 尚未实现 | 采样统计、极值搜索和事件区间分开建模 |
| 文档/自动化 | CSV；Python/C++ 与桌面共用计算 | HTML/PDF、带元数据导出、批量任务 |
| 大规模结果 | 预估行数、20 万行上限、后台取消 | 实测需求后增加分块存储及流式导出 |

STK 的报告内容允许选择数据提供器、重排字段、划分具有不同时间依赖的节，并分别设置单位和统计。对应到 OrbitForge，下一阶段应把“查询字段/区间”和“显示样式”分离；时间序列与 Access 区间需要不同结果结构，避免把区间强行展开成采样行。[内容与统计](https://help.agi.com/stk/Content/stk/report-06.htm)

STK 在结果窗口提供刷新、复制、查找、单位及导出设置。本轮先补齐对象入口、配置副本与表格复制，后续优先完善结果内的格式调整。仍使用简单默认 CSV，需要排版的报告另走文档导出。[结果操作](https://help.agi.com/stk/Content/stk/report_export.htm)

## 调研参考

- [STK Report & Graph Manager](https://help.agi.com/stk/Content/stk/report.htm)：报告/图形样式、对象、时间设置及生成入口。
- [STK 报告样式内容](https://help.agi.com/stk/Content/stk/report-06.htm)：数据元素、分节和统计配置。
- [STK 数据导出](https://help.agi.com/stk/Content/stk/report_export.htm)：报告导出工作流。
- [STK ReportCreate](https://help.agi.com/stk/Subsystems/connectCmds/Content/cmd_ReportCreate.htm)：自动化生成入口。
- [PySTK Results and graphs](https://stk.docs.pyansys.com/version/stable/user-guide/results-graphs.html)：数据提供器与结果访问。
