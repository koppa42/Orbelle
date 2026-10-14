# 轨道预报器

桌面卫星属性、C++ Runtime 与 Python 共用同一计算实现。预报器在一轮计算中固定输入，支持正向、反向和任意顺序查询。所有状态使用米、米/秒和 TAI `Epoch`，参考系随状态明确返回。

| 模型 | 输入 | 原生输出 | 适用范围 |
| --- | --- | --- | --- |
| Two Body | GCRF 瞬时位置/速度或经典根数 | GCRF | 理想二体轨道、基础设计 |
| J2 secular | 平均椭圆，含平近点角 M | GCRF | 一阶长期趋势 |
| J4 secular | 平均椭圆，含平近点角 M | GCRF | 含 J2、J2²、J4 的长期趋势 |
| SGP4 | 一组固定的 TLE，两行各 69 个 ASCII 字符 | TEME of date | 与 TLE 配套的近地/深空预报 |
| HPOP（初版） | GCRF 瞬时位置/速度或经典根数；力模型与积分设置 | GCRF | 静态球谐与可选太阳系第三体引力下的数值传播 |

此分工参考 [STK 预报器说明](https://help.agi.com/stk/12.7.1/Content/stk/vehSat_orbitProp_choose.htm)。新增模型不改变已有二体/J2 的模型常数或算法。

机动轨道可选[任务序列](missions.md)：从初始状态按顺序执行滑行和脉冲，滑行使用二体或 HPOP。任务向前定义，区间内支持任意顺序查询；点火时刻返回机动后状态。当前不包含有限推力和自动目标求解。

## HPOP

当前支持静态重力与太阳系第三体数值传播。通用地球轨道所需的完整力模型、积分高级能力与协方差目标见 [HPOP 与 STK 的对照及重构要求](hpop_alignment.md)；其中规划能力尚未进入本节的已实现范围。

参考 STK 的[力模型分层](https://help.agi.com/stk/Content/hpop/hpopForceMods.htm)和[积分设置](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)，在 GCRF 中以连续 TAI 秒积分 Cowell 六维方程。输入为瞬时状态，不能把 J2/J4 平均根数当作等价瞬时初值。当前支持：

| 配置 | 实现与默认值 | 边界 |
| --- | --- | --- |
| 地球引力 | 桌面新配置默认 EGM2008、21×21，缺少数据时首次打开设置自动下载；SDK 默认内置 EGM96 8×8、最高 20×20；可下载完整 EGM96 / EGM2008，或导入静态 `.gfc` / `.grv`；文件模型计算最高 360×360，0/0 为所选模型点质量 | 文件阶次与计算上限分别显示；没有系数时变、固体潮或海潮 |
| 第三体引力 | 日月、七行星及冥王星独立启用，默认关闭；DE440s 地心几何位置，GM 来源为 JPL DE / Cb file / User Specified | 每个积分阶段求值；覆盖不足报错，不外推 |
| 积分器 | RKF78 自适应；RK4 固定步长 | 尚无 VOP、RK89、Gauss–Jackson、Bulirsch–Stoer |
| 自适应容差 | 相对 1e-12，位置绝对 1e-3 m，速度绝对 1e-6 m/s | 分量误差尺度为 `atol + rtol × max(abs(old), abs(new))`；是局部数值误差，不是预报精度 |
| 步长 | RKF78 默认 0.001–60 s；RK4 默认 10 s | 输出采样间隔独立；RK4 不作误差控制 |
| 停止条件 | 最低地心高度（默认 0 m）、无法满足容差、非有限状态、资源上限 | 高度相对所选重力模型参考球面，不是大地高；不作再入事件定位 |
| 其他摄动 | 尚未计入大气阻力、光压、相对论和潮汐；脉冲机动使用任务序列 | STK 的完整力模型更多，不能据本阶段宣称长期 LEO/GEO 预报精度等价 |

EGM96 取自 [ICGEM 原始系数](https://icgem.gfz.de/getmodel/gfc/971b0a3b49a497910aad23cd85e066d4cd9af0aeafe7ce6301a696bed8570be3/EGM96.gfc)，完整文件 SHA-256 为 `5247a9e9c316dd2c8f8fd491d53be0e163cb5cb3676b021754240cc9e44cb43b`；使用 μ = 3.986004415e14 m³/s²、R = 6378136.3 m，参考 Lemoine et al., [NASA/TP-1998-206861](https://ntrs.nasa.gov/citations/19980218814)。`tools/data/import_egm96.py` 校验完整文件后提取 2–20 阶完全归一化系数，生成编译期表；正常构建和运行不联网。模型 ID 为 `egm96-static-tide-free-20-v1`，系数随程序固定。

球谐势采用完全归一化 Cartesian 实心球谐递推，以解析一阶自动微分求梯度，避免极点经纬度除零、差分误差及高阶未归一化系数溢出。每次力评估使用当前分析持有的 EOP 将位置旋转到 ITRF，计算引力后旋回 GCRF；惯性方程不另加离心或科里奥利项。即使输出 GCRF，非中心重力也必须有 EOP，其哈希进入结果来源。数据必须覆盖初始历元到场景时段的整个积分路径；目录选源和桌面准备均按该范围检查，计算中不换源或联网。

文件模型使用自己的 GM、参考半径及潮汐系统，不静默转换 tide-free / zero-tide 系数。资源保留完整文件，读取时仅将计算上限内的系数装入不可变 `GravityModel`。`HpopOptions.gravity_model` 保存目录选择 ID，`gravity_mu` 保存初始根数转换和报告所用 GM；准备时与文件核对，GM 不符时需重新选择模型并确认轨道输入。自定义文件可以更新系数，但不会偷偷重新解释原始根数。模型哈希与解析版本进入项目 LOCK，文件更新经确认后重建全部调用方，运行中的分析继续持有原资源。

RKF78 系数来自 Fehlberg [NASA TR R-287](https://ntrs.nasa.gov/citations/19680027281)，推进八阶解，以嵌入七阶解估计误差；不宣称逐位复现 STK 的控制器。从初始历元向前后独立建立完整 300 s 计算段，余段单独积分；改变采样、乱序、重复和并发查询不改变结果。共享缓存加锁，取消保留已完成段，不发布部分结果。每次查询最多 200000 次积分尝试，距离初始历元最多 366 天；这是资源边界，不是精度保证。显示插值不参与科学查询。

```python
satellite = project.objects.satellite("Numerical", orbit=of.orbits.Hpop(
    initial=of.orbits.Keplerian(epoch, u.km(7000), 0.01, u.deg(98.5)),
    gravity_degree=8,             # 省略 gravity_order 时与 degree 相同
    max_step_seconds=60,
))
analysis = project.prepare(data=data)  # 显式 DataSnapshot 或 DataCatalog
trajectory = analysis.states(satellite, step=u.s(30))
```

RK4 使用 `integrator="rk4", fixed_step_seconds=10`；点质量使用 `gravity_degree=0`，无需 EOP。`satellite.orbit.hpop` 是不可变 `HpopSettings`，可用 `dataclasses.replace` 修改后将新 `Orbit` 赋回 `satellite.orbit`；旧分析继续持有旧参数与数据。完整示例见 `examples/python/propagators.py`。

C++ 设置 `OrbitInput.propagator = OrbitPropagator::hpop`、`OrbitInput.hpop`（`HpopOptions`），通过 `Project::prepare(data)` 查询。低层工厂 `runtime::make_hpop_session(initial, options, environment, id)` 持有不可变 Frame/EOP、重力和第三体 Provider 的共享所有权。核心与不启用第三体的点质量模式无需 CSPICE。桌面选择 HPOP 后，“配置…”外层为力模型、积分器、协方差；力模型内分引力、阻力、太阳辐射压、其他四页。引力页将中心天体引力与第三体引力放在同一滚动页面；第三体区域用 Name / Use / Source / GM 四列表格配置日月、七颗行星及冥王星，支持搜索和独立的 Cb file / JPL DE / User Specified 来源。最低停止高度放在积分器页。阻力、太阳辐射压、其他模型与协方差尚未实现，页面只显示能力状态，不提供可保存为已启用的开关。切换内外页、调整窗口或单位不重建输入；校验失败返回相应页并定位字段。保存设置仅改变属性草稿，“应用轨道”才计算并发布。参数随项目 v17 保存。

独立回归使用 Orekit 13.1.2 Holmes–Featherstone 重力和 Hipparchus 4.0.2 DP853：五类轨道 × 三组阶次 × ±6000 s/±1 天共 60 个状态，检查位置每分量 0.15 m、速度 0.15 mm/s、初始加速度 5e-13 m/s²。参考刻意采用静态同轴重力系，隔离球谐与积分；真实 EOP 路由另由集成测试验证。尚无同配置 STK 导出数据直接对照，见 [HPOP 验证](../validation/hpop_protocol.md)。后续按完整截图清单接光压、空间天气、大气、潮汐、附加力、积分高级选项和协方差；未实现的力不提供无效开关。

## J4

`runtime/j4.hpp` 提供 `j4_mean_ellipse_state`、`j4_mean_ellipse_elements` 和 `make_j4_session`。`J4MeanElements` 与现有 `J2MeanElements` 使用同一数据布局；参数含义仍取决于选择的模型。转换函数只在辅助平均椭圆与 Cartesian 表示间转换，不进行瞬时根数到平均根数的物理拟合。

采用 Kozai 长期项：保持 a、e、i 不变，推进 Ω、ω、M；包含一阶 J2、二阶 J2 和一阶 J4。方程参考 Vallado (2013) §9.4 和 [SatelliteToolboxPropagators.jl 的 J4 实现](https://github.com/JuliaSpace/SatelliteToolboxPropagators.jl/blob/2fded936bbc911c4ecd3a48c2b9fdae7cc9c8e5e/src/propagators/j4.jl)。适用地心椭圆、近地点高于模型参考面，不含阻力、第三体、太阳辐射压和长/短周期项。

固定 EGM2008 tide-free 常数：μ = 3.986004415e14 m³/s²，R = 6378137 m，J2 = 0.0010826261738522227，J4 = -1.6198975999169731e-6。系数为未归一化形式 `Jn = -sqrt(2n+1) Cn0`，模型 ID 为 `earth-egm2008-j4-v1`。原有 J2 保留 IERS 2003 常数。

输出速度是完整长期轨迹的位置导数：包含平均角速度修正、节点进动和近地点进动，不能直接用辅助椭圆的 Kepler 速度。数值记录算法 `j4-secular-kozai-gcrf-derivative-v1`。

[STK 也定义 J4 为这些长期项](https://help.agi.com/stk/12.7.1/Content/stk/vehSat_orbitProp_2bodyJ2J4.htm)，但这不等于逐位复现 STK。[参考实现明确记录过 RAAN 的差异](https://juliaspace.github.io/SatelliteToolboxPropagators.jl/stable/man/propagators/j4/)。本实现保留公开方程的 J4 节点项符号，没有为拟合某个 STK 版本而改号；目前没有同配置 STK 导出数据的直接验收。比较时必须统一常数、平均根数定义、历元和速度约定。

## SGP4 与 TLE

`core::Tle` 保存原始两行；`core::parse_tle` 校验固定列、字符、校验和、编号一致性、范围和历元，支持 Alpha-5 编号。只接受标准 ephemeris type 0；两位年份以 57 分界，表示 1957–2056。遵循 [NORAD 格式说明](https://celestrak.org/columns/v04n03/)：第 000 天表示上一年 12 月 31 日，空白指数项表示零，整数允许前导空格。八位日小数精确换算为纳秒，不通过大浮点儒略日相减求秒差。

`providers::sgp4` 使用 Vallado 2023-05-09 算法，固定 WGS72 和 improved 模式。近地、深空、共振分支由模型自动选择，不另设“SDP4”输入类型。实现固定于 MIT 许可的 [python-sgp4 提交](https://github.com/brandon-rhodes/python-sgp4/tree/8126f773923c2902719d45aab8bdb5cce86cf39d)，C++ 原文、许可证和来源说明保存在 Provider 的 `third_party` 中。此 Provider 只依赖 Core，不依赖 Qt、CSPICE 或网络。

`make_sgp4_session(tle, time, object_id)` 必须显式注入 `ITimeProvider`，仅在初始化时把 TLE 的 UTC 历元解释为 TAI；后续使用连续 TAI 分钟差。该时间选择与 [DME/STK Components 的说明](https://help.agi.com/STKComponentsJava/Javadoc/agi-foundation-propagators-Sgp4Propagator.html)一致；跨闰秒时，与直接相减 UTC 儒略日的调用方式可能相差一秒。时间数据哈希进入 Session provenance。

原始例程会修改深空积分状态，每次查询复制已初始化记录，避免随机顺序或并发查询污染计算。初始化、衰减、非法轨道与非有限结果明确报错。TLE 原文随当前 `.orbitforge` 格式持久化；计算不自动换用另一组根数，也不在 Provider 内联网。

桌面选择 SGP4 后显示独立的粘贴/文件导入界面，可带一行卫星名称；拒绝含多组 TLE 的文件，导入失败保留当前草稿。显示编号、UTC 历元和名义周期。场景距离历元超过 14 天时提醒检查根数时效，这是交互提示，不是精度保证或模型硬有效期。切换预报器不把普通椭圆伪装成 TLE；原手动输入作为非活动草稿保留。重开 TLE 属性时，手动草稿以保存的 GCRF 状态恢复，不要求旧草稿历元仍有 EOP 覆盖。

当前尚不包含 OMM、多组 TLE 的自动选择、在线卫星目录或 TLE 拟合。数值力模型由独立 HPOP 路线推进；脉冲机动见[任务序列](missions.md)。

## TEME 与现有场景

`FrameId::teme` 表示 TEME of date，不能直接标记为 GCRF。ERFA Provider 提供 `teme_to_itrf` 和 `teme_to_itrf_state`：UT1 → GMST82 → PEF → 极移旋转（`Pom00`，s′ = 0）。状态变换包含 GMST82 的解析导数、UT1 变化率和极移变化率。GCRF 转换通过同一 ITRF 和当前 IAU 2006/2000A 模型完成。

`core::transform_state` 统一路由 GCRF、ITRF、TEME；其他参考系明确拒绝。报告、显示缓存和缓存之外的播放查询使用相同路由。SGP4 原生 Session 可不依赖 EOP；转换后的地图/三维/报告必须有覆盖查询时段的已确认 EOP。

SGP4 的约定输出速度并非其近似位置公式的精确导数。显示缓存单独以 0.125 秒位置差分建立 Hermite 切线（端点使用单侧差分，短区间缩小步长）；原生 Session 和报告继续保留标准 SGP4 速度。每段显示轨迹仍在 1/4、1/2、3/4 处验证 1 m 误差上限，显示缓存不进入科学计算。

项目 `make_orbit_session` / `make_project_scenario` 在有 SGP4 对象时要求传入时间 Provider，Python 为 `project.to_scenario(time)`。重建桌面项目时捕获已确认的时间数据，并重建 SGP4 Session；下载不会修改运行中的实例。

## 验证与限制

- 8 组 Vallado/CSSI 案例、157 个 TEME 状态，覆盖普通阻力、低偏心率、深空、低倾角、半日/同步共振、负时间；容限 1 mm、2 μm/s，由原始输出精度和平台浮点差异确定。
- J4 使用 60 位精度独立参考程序求解 Kepler 方程并对位置求导，验证 ±1 / ±30 天状态；另测零谐项二体极限、J2² 保留、圆/赤道/极轨/高偏心率和非法输入。参考生成器及固定值见 `tests/reference/propagators`。
- TEME 对照 Vallado 2006-6753 Rev 2 Appendix C。公开例使用单个 double 儒略日，造成约 22.790 μs 的 UT1 舍入和 13.2 mm 的位置残差；精确打印 UTC 的 60 位计算与本实现相差小于 1 μm。速度与原例存在 GMST 导数/恒定地球自转率约定差异，比较容限 0.2 mm/s；另验证数值导数和完整状态往返。
- 当前格式固定样本、往返读写及非当前版本拒绝；无时间/EOP、损坏 TLE、未应用草稿、并发与衰减错误路径。

这些检查验证实现一致性，不代表 TLE 的真实预测误差达到毫米级。实际误差取决于根数质量、历元距离、阻力和机动。

第三体配置、系统质心约定及 Cb 文件导入范围见[第三体引力](third_body_gravity.md)。
