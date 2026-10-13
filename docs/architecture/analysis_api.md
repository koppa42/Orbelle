# 固定输入的分析 API

本批次在[对象编辑 API](object_editing.md)之上接入共享 C++ Runtime 分析入口。Python 使用 `project.prepare()`，C++ 使用 `runtime::prepare(document, data)`。准备时捕获已提交对象、区间、传播器和显式选择的数据；后续对象修改、删除、文件替换或目录释放不会改变已有分析。

## Python 状态查询

```python
import orbitforge as of
from orbitforge import units as u

project = of.Project("Observation", of.Epoch(0), of.Epoch(300_000_000_000))
sat = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(
    initial=of.orbits.Keplerian(
        epoch=project.first, semi_major_axis=u.km(7000), eccentricity=0.001,
        inclination=u.deg(53), raan=u.deg(0), argument_of_periapsis=u.deg(0),
    ),
))

analysis = project.prepare()
state = analysis.state(sat, at=project.first, frame=of.FrameId.GCRF)
trajectory = analysis.states(sat, step=u.s(30), frame=of.FrameId.GCRF)
trajectory.to_csv("trajectory.csv")

# 显式历元保持顺序和重复项，不增加额外端点。
selected = analysis.states(sat, times=[project.last, project.first, project.last])

# 修改甚至删除当前对象后，旧分析仍可使用原句柄查询其捕获的对象。
sat.name = "Renamed"
project.objects.remove(sat)
original = analysis.state(sat, at=of.Epoch(0))
```

状态包含历元、地心原点、参考系、米和米每秒数值。二体、J2、J4 和 SGP4 复用现有传播器；地面站使用 WGS84 固定 ITRF 模型。当前 `state()` / `states()` 接受卫星和地面站，传感器位置查询其父卫星。GCRF 是 Python 查询的显式默认值，SGP4 原生输出为 TEME，地面站原生输出为 ITRF；跨参考系查询必须准备 EOP 数据，绝不把 TEME 当成 GCRF。

| 参数或行为 | 约定 |
| --- | --- |
| `at=Epoch(...)` | 单历元，返回一个 `CartesianState` |
| `step=u.s(30)` | 规则采样，包含起点和精确终点；不整除时追加较短的最后一步 |
| `first=...`、`last=...` | `step` 查询的可选子区间，可以为同一历元 |
| `times=epochs` | 与 `step` 互斥；空序列返回有完整类型信息的空结果 |
| `u.s()`、`u.ms()`、`u.ns()` | 转成精确的 int64 纳秒；拒绝亚纳秒精度、非有限值和溢出，查询步长必须为正 |
| 查询范围 | 全部历元必须位于准备区间，不越界传播 |
| 单次状态序列 | 最多 200000 个样本，分配前检查；更长序列拆分查询 |
| 对象范围 | 同一项目捕获的对象；拒绝其他项目同 ID 句柄和准备后新增的对象 |

时间零点仍为 **2000-01-01 12:00:00 TAI**，不是 J2000 TT。采样在整数纳秒上进行，避免大历元与小步长叠加丢精度。项目自身现有的区间有效性约束仍然适用。

## 数据与确认计划

推荐从已经安装数据的目录准备分析，Python 和 C++ 与桌面共用完整覆盖优先、来源优先级和有限外推的选择规则：

```python
catalog = of.DataCatalog("local/shared-data")
analysis = project.prepare(data=catalog)
trajectory = analysis.states(sat, step=u.s(30), frame=of.FrameId.ITRF)

# 可选：显式修改这个目录的共享策略，之后的准备才会使用。
catalog.preferred_eop = of.EopProduct.C04
catalog.set_extrapolation_days(of.EopProduct.C04, 3)  # 0 关闭；最多 30 天

# 也可以提前取得固定资源，传给一个或多个分析。
data = catalog.snapshot(project.first, project.last)
analysis = project.prepare(data=data)
```

目录路径由调用方显式提供；选源不联网、不读取其他目录、不更改当前文件指针。默认 C04 优先；先按优先级找完整覆盖整个区间的当前来源，全部不足时再考虑各来源允许的外推，不拼接两份文件。原始数据已经覆盖时不会启用外推，因此未使用的外推设置不改变计算哈希。`catalog.snapshot()` 允许单历元区间，反向区间报错；分析项目仍遵守自身的时段约束。

选源在一次 SQLite 读取中捕获当前 UTC、EOP 指针、优先级、外推策略和旧目录兼容索引，然后校验并完整加载资源。其他进程发布新数据、目录对象释放或文件随后移除，都不会改变已有快照或确认计划。当前 UTC 缺失、损坏或失效时明确报错；仅目录尚未选择 UTC 时使用内置表。当前 EOP 文件失效时可选择另一个可用来源，并通过 `data.selection_notes` 说明；有显式当前指针时不会回退到该产品的旧缓存。没有当前指针的旧目录按有效获取记录 / 插入顺序选择每种产品的当前候选。覆盖不足的错误包含所需 TAI 纳秒区间、候选原始范围、外推天数及失效原因。

低层 `install_eop()` / `import_c04()` / `import_finals()` 只安装资源。需要从脚本指定刚安装的文件时，使用 `catalog.activate_eop(entry.lock_sha256)`；它检查该文件的 UTC 依赖后更新相应当前指针，不改下载记录和正在运行的分析。桌面下载与导入继续通过原有校验和发布流程管理当前文件。`preferred_eop` 与 `set_extrapolation_days()` 使用数据中心的同一组目录设置。

无外部时间依赖的原生状态查询不需要 EOP、联网或图形资源。SGP4 显式选择时间表：

```python
analysis = project.prepare(data=of.DataSnapshot.builtin_time())
# 只需要目录当前时间表时，不要求安装 EOP：
data = catalog.snapshot(project.first, project.last, earth_orientation=False)
analysis = project.prepare(data=data)
```

参考系转换显式加载本机文件：

```python
data = of.DataSnapshot.load(
    eop_manifest="data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json",
    # 可选；未指定时，此次显式加载使用程序内置闰秒表。
    time_file="data/samples/time/iers_leap_seconds_2026_07.dat",
    extrapolation_days=0,
)
analysis = project.prepare(data=data)
```

`DataSnapshot.load(time_file=...)` 也可只加载时间表。文件在加载时完成解析与校验，计算不重新打开文件。`data.time_info`、`eop_info` 和 `data_lock` 提供所选资源的覆盖、策略和身份。这个类型表示已选择的运行时资源，不增加历史版本界面。

`project.plan_prepare(data=data)` 固定对象输入、修订、数据和传播会话，先检查资源完整性与整个准备区间的覆盖。有限 EOP 外推为 0–30 天，0 表示关闭；它不延长 UTC 表有效期。无法覆盖或时间失效时立即抛出相应错误，不能通过同意更新绕过。

所选数据与项目记录的相同角色比较，身份或解释策略不同则列入 `plan.data_changes`。新项目没有相应记录时可直接准备；未选用的旧 EOP 不妨碍纯惯性轨道查询。直接 `project.prepare()` 遇到变化时抛出 `of.DataChangedError`，其 `plan` 是本次已固定的计划。

```python
try:
    analysis = project.prepare(data=data)
except of.DataChangedError as error:
    plan = error.plan
    # 应用展示 plan.data_changes，并取得本次明确决定。
    # 用户同意后调用；拒绝则不继续，保留已有分析。
    analysis = plan.prepare(accept_data_changes=True)
```

上例中的接受调用应放在应用的确认分支内。库本身不弹窗、不联网、不替调用方同意更新。接受仅影响这次调用；计划不会记住“以后都同意”。如果项目修订已变化，计划抛出 `RevisionConflictError`，需要重新生成。Python 编辑上下文内不能准备分析；已生成的旧分析仍可查询。

`plan_prepare(data=catalog)` 与直接传入快照使用同一确认流程，固定选源时对应的项目修订。准备成功不修改项目来源记录，不把计算结果写回项目。`plan_open()`、`save(..., analysis=...)` 和桌面接入新的准备计划尚待完成；普通 `Project.open()` 仍只读取保存的输入和来源信息。

## 结果与导出

`Trajectory` 独立持有结果，分析或项目释放后仍有效。名称和修订来自准备时的快照。`input_sha256` 记录对应对象状态输入及分析区间，排除名称、样式、无关对象及地面站最低仰角；`data_sha256` 仅记录该状态查询实际使用的依赖。未转换的二体/J2/J4 GCRF 状态和固定站点 ITRF 状态使用空数据锁，SGP4 TEME 依赖时间表，跨参考系状态依赖时间与 EOP。

```python
len(trajectory)                              # 样本数
trajectory.position.values                  # SI 三维向量元组
trajectory.epoch.tai_nanoseconds             # 精确整数元组
xyz = trajectory.position.to_numpy(unit="km")
velocity = trajectory.velocity.to_numpy(unit="km/s")
table = trajectory.to_pandas()
```

NumPy 数值列始终为 `(N, 3)`，包括 `(0, 3)` 和 `(1, 3)`；时间列为 int64 TAI 纳秒。pandas 保留整数 `epoch_tai_ns` 列和 SI 数值列，将单位、参考系、原点及来源放在 `attrs`。这些转换返回副本；不创建 `datetime64` 时间索引、不静默降精度。NumPy 和 pandas 均为可选依赖，缺少时仅相应转换方法报 `ImportError`。

CSV 使用精确的整数 `epoch_tai_ns`、标明 SI 单位的状态列以及原点和参考系列，复用现有报告格式化与原子发布实现，不重新计算。默认拒绝覆盖；明确使用 `overwrite=True` 才替换已有目标。空结果仍写完整表头。

## C++ 入口与验证

```cpp
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
using namespace std::chrono_literals;

// document 为 application::ProjectDocument，satellite 是其 ObjectRef。
auto analysis = orbitforge::runtime::prepare(document);
auto trajectory = analysis.states(satellite, 30s, orbitforge::core::FrameId::gcrf);
orbitforge::runtime::export_trajectory_csv(trajectory, "trajectory.csv");

orbitforge::runtime::DataCatalog catalog{"local/shared-data"};
auto earth_fixed = orbitforge::runtime::prepare(document, catalog);
auto state = earth_fixed.state(satellite, document.snapshot().project->first,
                               orbitforge::core::FrameId::itrf);
```

Runtime 位于 Application 之上，分析不让对象模型反向依赖 Provider。`ProjectSnapshot` 保留进程内项目范围；`snapshot.read(ref)` 从该修订读取对象，原对象删除后仍有效。C++ 的 `AnalysisPlan` 和 `DataChangedError::plan()` 与 Python 确认行为一致。

分析构建和查询可在 Python 释放 GIL 后运行。当前为同步状态查询接口，尚未提供通用任务取消、`analysis.access()`、新的报告模板或桌面缓存发布；原有 Access/报告入口继续可用。后续按[整体设计](object_api_design.md)逐批迁移。

验证包括 C++ `Analysis.*`、Python `test_analysis.py`、现有传播/时间/哈希回归，以及安装后的 SDK 消费端。覆盖文件移除后的资源存活、同 ID 跨项目拒绝、旧结果存活、确认计划过期、有限外推、UTC 失效、整数极限附近采样、CSV 覆盖保护及可选数组适配。可运行示例见 [analysis.py](../../examples/python/analysis.py)。

目录选源由 C++ `CatalogAnalysis.*`、Python `test_catalog_analysis.py` 和桌面数据中心回归共同验证：覆盖优先级、外推上限加 1 ns、损坏的当前文件、UTC/EOP 跨进程原子更新、确认期间切换数据及安装后的 SDK 入口。示例可使用 `analysis.py --data <目录>` 验证 ITRF 输出。
