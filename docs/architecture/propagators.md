# 轨道预报器

桌面卫星属性、C++ Runtime 与 Python 共用同一计算实现。预报器在一轮计算中固定输入，支持正向、反向和任意顺序查询。所有状态使用米、米/秒和 TAI `Epoch`，参考系随状态明确返回。

| 模型 | 输入 | 原生输出 | 适用范围 |
| --- | --- | --- | --- |
| Two Body | GCRF 瞬时位置/速度或经典根数 | GCRF | 理想二体轨道、基础设计 |
| J2 secular | 平均椭圆，含平近点角 M | GCRF | 一阶长期趋势 |
| J4 secular | 平均椭圆，含平近点角 M | GCRF | 含 J2、J2²、J4 的长期趋势 |
| SGP4 | 一组固定的 TLE，两行各 69 个 ASCII 字符 | TEME of date | 与 TLE 配套的近地/深空预报 |

此分工参考 [STK 预报器说明](https://help.agi.com/stk/12.7.1/Content/stk/vehSat_orbitProp_choose.htm)。新增模型不改变已有二体/J2 的模型常数或算法。

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

当前尚不包含 OMM、多组 TLE 的自动选择、在线卫星目录或 TLE 拟合。下一阶段可先增加 OMM 与目录导入，再扩展外部星历；数值积分、力模型和机动应走独立的数值预报器路线，不能把它们命名为已对齐 STK HPOP。

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
