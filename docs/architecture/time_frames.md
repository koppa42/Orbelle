# P0 时间与参考系接口

ERFA 2.0.1 与 IERS C04 Provider 通过 Runtime 供 C++ SDK 和独立 Python 包调用。Core 只定义值类型、接口及矩阵应用规则，仍可不带第三方库独立构建。

## 时间表示与范围

`Epoch` 继续使用 int64 TAI 纳秒，零点为 **2000-01-01 12:00:00 TAI**。`UtcDateTime` 的日期、时分秒和纳秒字段必须显式传入 `ITimeProvider::from_utc`；没有把 UTC 当成连续 86400 秒一天的浮点时间，也没有隐式解析字符串。

`make_builtin_time_provider()` 使用仓库固定的 IERS Bulletin C 72 闰秒快照。清单记录来源、原始文件 SHA256 和有效期。CMake 先校验原始字节，再生成只读记录表；wheel/SDK 同时携带原文与清单，运行时无需源码目录或网络。`.gitattributes` 禁止 Git 改写该数据文件的换行字节。

快照覆盖 **[1972-01-01 00:00:00 UTC, 2027-06-28 00:00:00 UTC)**。官方文件只给出到期日期，本项目保守地取到期日零点为排他上界。超出范围抛出 `TimeDataRangeError`，Python 中继承 `ValueError`；不使用 ERFA 内建的旧表兜底，也不自动延长有效期。日期非法或不存在的 `second=60` 抛出 `std::invalid_argument`，Python 映射为 `ValueError`。

ERFA 的 `eraCal2jd` / `eraJd2cal` 提供日历换算。UTC↔TAI 的偏移选择与正闰秒纳秒记账使用整数，避免经过双精度 JD 丢失原始纳秒。当前编译数据仅支持 1972 年以后、每次增加一秒的已公布历史记录；不支持 1972 年前的 UTC 频率调整。`load_time_snapshot(file)` 可加载 IERS Leap_Second.dat，每个实例持有独立只读记录及有效期，验证正／负单秒变化；负闰秒删除的 UTC 秒不可输入，正闰秒保留 `23:59:60`。内置编译表仍校验已公布的正闰秒历史。代码不调用 `eraSetLeapSeconds`，不改变 ERFA 全局数据。

`julian_date(epoch, TimeScale::tai/tt)` 返回带时间尺度标签的两部分 JD。内部算法应分别传递 `day` 和 `fraction`，不要先求和。TAI→TT 使用 `eraTaitt`，偏移为 32.184 s；它不依赖 UTC 表的覆盖范围。两部分 JD 属于浮点算法输入，不承诺覆盖整个 Epoch 范围的无损反向转换，目前也没有从任意浮点 JD 构造 Epoch 的 API。

J2000 TT 对应本工程的 `Epoch(-32'184'000'000)`，对应 UTC 为 2000-01-01 11:58:55.816。TAI 零点对应 UTC 2000-01-01 11:59:28。两者在测试中分别验证。

## 地球定向与位置旋转

`IFrameProvider::gcrf_to_itrf(epoch, eop)` 使用 IAU 2006/2000A 模型，分解为岁差章动、地球自转和极移，并应用观测天极偏移。`EarthOrientationSample` 必须提供：

| 字段 | 含义 |
|---|---|
| `epoch` | 与请求严格相同的 TAI 历元 |
| `ut1_minus_tai_seconds` | UT1−TAI，秒；不能直接填 DUT1（UT1−UTC） |
| `polar_motion_x_rad`、`polar_motion_y_rad` | ERFA/IERS 约定的极移 xp、yp，弧度 |
| `celestial_pole_dx_rad`、`celestial_pole_dy_rad` | 天极偏移 dX、dY，弧度；使用零值时也须显式给出 |
| `source_id` | 该采样值的数据来源或明确的合成样例标识，不能为空 |

若已有 DUT1，需要先按 `UT1−TAI = DUT1 − (TAI−UTC)` 转换。旋转接口接收已在指定历元求值的样本，没有缺省的零 EOP。输入检查有限数值、`|UT1−TAI| < 86400 s`、`|xp|, |yp| <= π/2` 和 `|dX|, |dY| <= 0.01 rad`；这些是宽松的数值定义域检查，不是数据质量认证。

返回值 `PositionRotation` 包含历元、Earth 原点、源/目标参考系、行优先矩阵、模型 ID 和 EOP 来源。使用列向量约定 `r_ITRF = R * r_GCRF`；逆位置变换使用转置矩阵。调用者应保证传入的位置属于该结果标注的历元、原点和源参考系。

GCRF/ITRF 表示地心 GCRS/ITRS 的轴向关系，没有实现特定 ITRF 实现版本间的站坐标变换。模型 ID 为 `erfa-2.0.1-iau2006-2000a-cpo-model-s`：`eraXys06a` 求理论 X/Y/s，X/Y 加入观测 dX/dY 后调用 `eraC2ixys`；依 IERS 2010 §5.5.3 保留理论 s。地球自转角与极移分别使用 `eraEra00`、`eraPom00`/`eraSp00`。

## 本地 EOP 快照

`runtime::load_eop_snapshot(manifest_path)` 读取调用者指定的本地清单与同目录数据，返回拥有数据的只读 `IEarthOrientationProvider`。Python 接受 `str` 或 `pathlib.Path`。加载一次后不再访问原文件，原目录移动或文件修改不影响已加载对象；再次加载会重新校验。运行时不联网、不自动寻找替代数据，也不外推。

格式限定为 IERS **20 C04、0h UTC、dX/dY、21 列**；使用 xp/yp 变化率，故起始日期要求不早于 1984-01-01。数据必须逐日连续、日期/MJD 一致、数值有限、参数处于支持域且不确定度非负。清单和数据最大为 64 KiB、16 MiB；路径不能跳出快照目录。先对读入字节计算 SHA256，再解析同一字节缓冲区。SHA256 用来检查数据一致性，并不证明来源身份。

清单 schema 1 的固定字段如下；额外字段或未知版本直接报错：

| 字段 | 约定 |
|---|---|
| `schema_version` | 整数字面量 `1` |
| `snapshot_id` | 非空版本标识 |
| `format` / `reference_frame` | `iers-20-c04-0h-dxdy` / `ITRF2020` |
| `data_file` | 同目录文件名，不含路径 |
| `sha256` | 数据原始字节 SHA256，小写十六进制 |
| `source_url` / `source_sha256` | 原始下载来源及完整原文件 SHA256；仅作为来源记录 |
| `retrieved_on` | 获取日期，导入工具要求 ISO 日期 |
| `first_mjd` / `last_mjd` | 整数 UTC 午夜节点，至少两个；均包含在覆盖区间内 |
| `interpolation` | `c04-hermite-tai-v1` |

`tools/data/import_c04.py` 从已有官方文件截取日期窗口，在所有校验通过后创建新快照目录，拒绝覆盖已有目录。加载器会用时间 Provider 校验选中节点，因此实际范围还受闰秒快照限制。`EopDataError` 表示加载/格式问题；`EopCoverageError` 表示查询越界，二者在 Python 中均继承 `ValueError`。

插值自变量是连续 TAI 秒。先在每个节点把 DUT1 换成 UT1−TAI，再进行三次 Hermite 插值。节点 UT1−TAI 斜率取 `−LOD/86400`，xp/yp 斜率来自 C04 的角秒/日字段；dX/dY 使用相邻节点割线估计共享斜率，端点使用单侧割线。实际闰秒日节点间距为 86401 秒。接口 `at(epoch)` 同时返回 `EarthOrientationSample` 和对 TAI 秒求导的 `EarthOrientationRates`。

覆盖范围是首节点至末节点的闭区间，末日只覆盖 **00:00:00 这一节点**。`EopDataInfo` 记录范围、散列、插值版本和所用时间快照。采样 `source_id` 还包含范围与 `subdaily-tides=none`：本实现没有增加日内海潮/天体潮汐 EOP 改正。范围参与来源标识，因为改变端点会改变 dX/dY 的边界斜率。

仓库附带 2016-12-28 00:00:00 至 2017-01-04 00:00:00 UTC 的八节点历史样例，目的是验证 2016 闰秒，不能用作当前历元的数据。

## 完整状态变换

`gcrf_to_itrf_state(epoch, eop_state)` 返回 `StateTransform`。其 `apply` 使用 `r'=Rr`、`v'=Rv+R_dot*r`，`inverse` 使用 `r=Rᵀr'`、`v=Rᵀ(v'−R_dot*r)`。输入 `CartesianState` 的历元、原点和参考系必须与方向相符，位置/速度必须有限；输出保留历元和 Earth 原点并更新参考系。

令 `R=P·Z·C`，则 `R_dot=P_dot·Z·C+P·Z_dot·C+P·Z·C_dot`。自转角导数解析计算，包含 UT1−TAI 的斜率。P/C 导数用 1 秒步长的五点中心差分；EOP 在请求点按已提供斜率做局部线性展开，TT 同时变化。差分不在快照范围外查询 EOP，所以两端节点可使用完整状态接口。导数模型 ID 为 `analytic-era-plus-polar-cip-fd4-1s-local-tangent`。

已在半径 ≤42,164 km、速度 ≤8 km/s 的验收样例中比较完整变换位置的时间差分，并用 Orekit 独立天文级数校验。数值结果与限制见[验收记录](../validation/p0_eop_state.md)；不把这组数值容限当成真实地球定向或轨道的绝对精度承诺。

## 装配与可选构建

`ORBITFORGE_WITH_ERFA` 默认关闭；`time-frames` CMake 预设和标准 wheel 构建显式开启。Runtime 装配 Erfa 和 Iers，两者均只依赖 Core 接口；Iers 通过注入的时间 Provider 换算节点。Erfa 私有链接 `erfa::erfa`，Iers 私有使用 nlohmann-json 3.12.0 和 PicoSHA2 1.0.1。Core/Application 不依赖具体 Provider。

Runtime 始终公开能力查询 `time_frames_available()`；禁用 Provider 的构建调用相应工厂会抛出明确的错误。Python 绑定通过 Runtime 装配，公开头文件不泄漏 ERFA 类型。当前 Provider 只读、无可变缓存；调用的 ERFA 函数也不访问可变闰秒表。

闰秒表可通过 `load_time_snapshot` 在运行时独立加载；桌面数据中心可联网更新，既有实例保持不变。EOP 通过 `load_eop_snapshot_with_time` 或带 `time_file` 的 DataCatalog 导入绑定对应时间数据。TAI↔TDB 与太阳/月球几何状态已由独立的 [CSPICE Provider](spice.md) 提供；ERFA 的 `julian_date` 仍只接受 TAI/TT。统一数据目录与 EOP／UTC 下载更新已实现；TCB、TEME 以及星历状态到 GCRF 的完整模型转换尚未实现。

## 依据

- [ERFA 2.0.1 源码](https://github.com/liberfa/erfa/tree/v2.0.1)。
- [eraC2t06a 的模型与矩阵约定](https://github.com/liberfa/erfa/blob/v2.0.1/src/c2t06a.c)。
- [eraTaitt 的时间尺度约定](https://github.com/liberfa/erfa/blob/v2.0.1/src/taitt.c)。
- [IERS 原始闰秒表](https://hpiers.obspm.fr/iers/bul/bulc/Leap_Second.dat)，项目使用下载后的固定快照，后续网址内容可能更新。
- [IERS 20 C04 格式与变化率说明](https://hpiers.obspm.fr/iers/eop/eopc04/readme)。
- [IERS Conventions 2010，第 5 章](https://iers-conventions.obspm.fr/content/chapter5/icc5.pdf)。
- [IERS UT1 与 LOD 关系](https://hpiers.obspm.fr/eop-pc/earthor/ut1lod/UT1.html)。
