# HPOP 大气阻力

桌面阻力页、C++ `DragOptions` 和 Python `orbits.DragSettings` 共用 Core 模型。当前实现球形阻力、Jacchia–Roberts / NRLMSISE-00 密度、手动/文件空间天气、低空拼接、近似高度及地心视太阳位置；低层密度接口支持原生日 Ap 和七项历史 Ap；自然三次样条地磁更新已接入；保持窗口均值的插值、月预报及风场尚未实现。[STK 阻力设置](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)是功能与输入含义的对齐依据；尚未取得 STK 同配置数值输出。

## 力与参考系

在 ITRF 中计算 `a = -0.5 * rho * Cd * (A/m) * |v_rel| * v_rel`，单位为 kg/m³、m²/kg、m/s、m/s²。大气与地球共转且无风；`v_rel` 来自完整 GCRF→ITRF 状态变换，包含旋转率项。只旋转惯性速度会错误地把地球自转计入相对速度。每个积分阶段重新求 UTC、EOP、太阳和密度，再将加速度旋回 GCRF。

默认密度高度使用精确 WGS84 椭球高，半长轴 `a=6378137 m`、扁率 `f=1/298.257223563`。`use_approximate_altitude=True` 启用 [STK 文档中的椭球径向近似](https://help.agi.com/stk/Content/hpop/approximateAltitudeComputation.htm)：`h=R−a(1−f)/sqrt(1−f(2−f)cos²φ)`，其中 `R` 是地心距，`φ` 是地心纬度。赤道和两极采用其有限极限；没有把大地纬度代入该公式。

开关只替换密度、模型有效高度检查和高低空拼接使用的高度；大地经纬度保持精确定义，位置报告仍为精确 WGS84 大地高。`DragEvaluation.geodetic` 返回精确经纬高，`density_altitude_m` 单独返回实际密度高度。当前仍计算精确纬度，因此不承诺此开关带来 STK 所述的性能收益。它不改变积分器的最低地心高度阈值。太阳赤纬与当地太阳时由同一 ITRF 太阳矢量求得，太阳时角是向东相对太阳子午线的夹角。地心奇点、非有限输入和域外高度明确报错。

Cd 为带符号经验系数，范围 −100…100；面积质量比为 0…9999 m²/kg，与光压参数独立。这两项范围对应 [STK HPOP Drag 命令](https://help.agi.com/stk/12.10.0/Subsystems/connectCmds/Content/cmd_HPOPDrag.htm)。负 Cd 产生反向经验加速度；正常阻力使用正 Cd。零面积或零 Cd 不改变已启用模型的数据依赖。

## 密度与天气

Jacchia–Roberts 适用 90…2500 km，保留 NASA GMAT 标量模型的拟合常数及分段公式，并处理正午、午夜、极点、太阳赤纬为零的有限极限。GMAT 外层 `Density` 不开放 90…100 km，本实现使用其原有标量公式开放这一高度段。NRLMSISE-00 适用 0…1000 km，移植 Orekit 13.1.2 的双精度计算和表格，使用 `gtd7d` 总质量密度，包含异常氧；默认变化项开启，使用日 Ap。源码来源和 Apache-2.0 许可见 [模型说明](../../modules/core/src/propagation/atmosphere/README.md)。所有临时状态属于单次调用，模型可并发使用。

手动天气来源为常量输入：日 F10.7、平均 F10.7（sfu）、Kp。NRLMSISE 将手动日值作为前一日 F10.7，将平均值作为模型平均通量；没有隐含的历史查询。Kp 使用标准 1/3 刻度的 28 项 Ap 表，非表格刻度线性插值，Kp=3 对应 Ap=15。界面/API 当前限制通量 `(0,1000]` sfu、Kp `[0,9]`，这些限制与 STK 命令允许的全部范围不同，不能据此宣称所有参数范围已对齐。低层 `AtmosphereInput` 使用 UTC 公历日期 1600…2600；闰秒合法性在 HPOP 中由实际时间 Provider 认证，独立标量调用方负责保证其 UTC 真实有效。

JR 可选择 NRLMSISE-00 作为低空模型。低于 90 km 使用低空模型；在 `90 km…90 km+拼接范围` 内，以 `w=t²(3−2t)` 对质量密度加权，在其上使用 JR。正范围拼接具有连续的一阶权重；零范围表示明确的硬切换。有效拼接范围至多 910 km，防止低空模型查询超出 1000 km。NRLMSISE 为主模型时低空设置不生效，但合法草稿仍保存。该拼接定义为 OrbitForge 的公开约定，仍需 STK 数值核对。

## 原生 Ap 密度接口

`evaluate_atmosphere(NRLMSISE00, input)` 可通过 `AtmosphereInput.daily_ap` 直接使用原生日 Ap，或通过 `ap_history` 使用七项历史数组；二者互斥，均省略时沿用 Kp→日 Ap。显式 Ap 模式不读取 Kp。所有 Ap 值必须有限且在 `[0,400]` 内，JR 拒绝显式 Ap，避免误把 Ap 当成 Kp。

历史数组按模型原生顺序保存：日 Ap、当前三小时 ap、此前 3 / 6 / 9 小时 ap、此前 12–33 小时八项 ap 的均值、此前 36–57 小时八项 ap 的均值。输入归调用方所有，模型按值使用；每次计算的状态独立。日模式设置 `SW[9]=1`，历史模式设置 `SW[9]=-1`，即使七项相同也不能把历史模式简化为日模式。原生定义见[原作者接口](https://git.linta.de/~brodo/nrlmsise-00.git/plain/nrlmsise-00.h?id=bc9a2feba4344e74201281e563332688a4d09cc3)。

```python
inputs = of.AtmosphereInput()
inputs.utc = of.UtcDateTime(2000, 2, 29, 12, 30, 0)
inputs.geodetic = (0.4, 0.6, 400_000)  # 经度、大地纬度、椭球高；rad、rad、m
inputs.f107, inputs.f107_average = 150, 150
inputs.ap_history = (40, 400, 100, 50, 20, 7.125, 3.75)
density = of.evaluate_atmosphere(of.AtmosphereModel.NRLMSISE00, inputs)
```

这个入口也供空间天气装配调用；标量密度接口本身不读取文件、不补齐缺值。HPOP 的文件模式在每个积分阶段通过不可变 `SpaceWeather` 取样；JR 与低空 NRLMSISE 分别使用自己的时间滞后。

## 空间天气文件

支持 CelesTrak 原始 31 列 CSV 和 130 列 `CssiSpaceWeather` 定宽文件，格式依据 [CelesTrak 说明](https://celestrak.org/SpaceData/SpaceWx-format.php)。原始全文及 SHA-256 保留，解析区分观测/预报、观测/1 AU 调整通量、中心/尾随 81 日均值、日 Ap 与三小时 ap。空字段保持缺失。观测 Kp 的 53 表示 5⅓；预报中由 Ap 推导的非三分之一值（如 22、24）按十分之一刻度保留。

桌面“阻力 → 太阳通量 / 地磁”可选手动或文件、每日、三小时或三小时自然三次样条更新，以及 Ap 或 Kp 来源。文件由数据中心下载/导入 CelesTrak `SW-All.csv`，API 不隐式联网。`DragSettings.weather_model` 是目录选择 ID，空字符串表示手动，`geomagnetic_update` 为 `daily` / `three_hourly` / `three_hourly_cubic_spline`，`geomagnetic_source` 为 `ap` / `kp`。也可显式 `DataSnapshot.with_space_weather("celestrak", path)`。

按照 [STK Drag 文件说明](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)，日与中心 81 日观测通量在 20:00 UTC 节点间线性取样。滞后采用 [AGI JR 定义](https://help.agi.com/STKComponents/html/T_AGI_Foundation_Celestial_ScalarDensityJacchiaRoberts.htm)及 [MSIS-2000 定义](https://help.agi.com/STKComponents/html/T_AGI_Foundation_Celestial_ScalarDensityMsis2000.htm)：两者日通量滞后 86400 s；JR 平均通量也滞后 86400 s、地磁滞后 24120 s；NRL 平均通量及地磁不滞后。这是公开组件文档依据，尚未完成 STK HPOP 同场景数值验收。

积分步在日/三小时地磁更新处截断（JR/低空 NRL 各自的滞后窗口均计入），RK 端点使用该步内部一侧的天气值；正反向均不跨跳变混合密度。滞后按 TAI 实际秒数，日/三小时窗口按 UTC；正闰秒属于当天最后一个三小时窗口。NRL 三小时模式组装上述七项 Ap 历史；选择 Kp 时先逐项转换为 Ap 再求历史均值。日模式使用文件日 Ap 或由文件日 Kp 转换，不能从当日八项重新计算并替换原始日值。

准备时检查完整传播路径所需的每个日期、通量插值节点和全部历史槽，包括初始历元早于场景的情况。缺字段、内部断档、域外查询均报错，不外推或零填充。文件中月预报记录保留，但当前每日/三小时模型拒绝使用它们；原始首末日期不是可传播范围。3-Hourly Interp（保持三小时均值）、月预报、CSSI predicts 和 FXM 仍待实现。

## 三小时自然三次样条

`three_hourly_cubic_spline` 对应 STK 的 3-Hourly Cubic Spline 选择。STK 公开说明采用自然三次样条，但没有公开节点落点、断档端点和越界策略。以下是 OrbitForge 的明确计算约定，已对照独立 SciPy 与 GMAT；这些细节仍需同配置 STK 数值验收，不将其称为完全复现。

将每个三小时值放在该 UTC 窗口中心（01:30、04:30、…、22:30）。在连续、非月预报且该来源无缺值的序列上，一次性求自然三次样条，首末节点的二阶导数为零；两节点退化成线性，孤立节点仅允许精确命中。Ap 与 Kp 各有自己的有效序列，缺失日期/字段和月预报均断开序列，不跨缺口连接，也不外推。远处节点可能影响同一完整序列的曲线，因此更新文件仍须重新准备和确认；运行中的数据不可变。

样条的独立坐标是等距 UTC 三小时窗口编号。每对节点间以真实 TAI 时差求局部分数，跨闰秒的节点间隔实际为 10801 s。该时间映射连续，样条对窗口坐标 C²；不承诺闰秒处对 TAI 的导数连续。JR 仍先施加 24120 s 地磁滞后。选择 Ap 时先插值 Ap，再转为 Kp；选择 Kp 时直接插值 Kp，不能先转换节点再声称曲线相同。

自然样条可能越过原始节点范围。结果超出 Ap `[0,400]` / Kp `[0,9]` 时明确报错；仅容忍不超过 `64 × double epsilon × 上限` 的舍入偏差并收回边界。准备计算逐段检查端点及导数的实根，能发现两端合法而内部越界的情况。错误提示可改用每日/三小时取样或完整数据，不静默改变已选插值算法。

依照 STK 对 MSIS 不使用插值的说明，NRLMSISE 在该选择下沿用未插值的七项 Ap 历史、`SW[9]=-1`，其有效身份等同三小时取样。JR 与低空 NRL 混合时，两者仍分别取样；前者使用样条，后者使用原生历史。设置草稿保留选择，手动天气或禁用阻力时该项不改变结果身份。现有 UTC 步边界策略仍生效。

C++ / Python 的枚举新增 `three_hourly_cubic_spline` / `THREE_HOURLY_CUBIC_SPLINE`；高层 `DragSettings` 使用同名字符串。项目 v29 / OBJS20 保存枚举 2，拒绝旧格式和未知值；参考样例独立构造并覆盖未启用草稿。JR 文件样条的算法标识包含 `natural-utc-midpoint-runs-v1;overshoot=error`。

## 数据与持久化

阻力即使只启用点质量引力，也需要覆盖完整传播路径的 UTC、EOP、Frame Provider 和太阳星历。启用后不因缺项而退回无阻力；UTC 内容哈希必须与 EOP 保留的时间来源一致。数据快照保留这些 Provider 的共享所有权，编辑项目或更新数据不改变已准备的计算。

默认使用几何太阳。`use_apparent_sun_position=True` 使用 CSPICE 对地球接收观测的 `LT+S` 太阳方向；此约定影响密度中的当地太阳时/赤纬，不修改第三体引力或光压的几何太阳。准备计算时额外认证区间起点前 600 s 的太阳与地球核覆盖，边界不足明确拒绝。自定义星历必须提供相应查询和区间认证接口，不能将几何方向冒充视方向。

项目格式为 v29 / OBJS v20，只读写当前版本。禁用模型时保存合法参数草稿，计算身份忽略这些草稿；启用时计入实际密度模型、天气、Cd、面积质量比、高度算法、太阳方法及 UTC/EOP/星历来源。NRLMSISE 主模型的低空草稿和未启用的拼接范围不改变结果身份。

```python
import orbitforge as of

drag = of.orbits.DragSettings(
    enabled=True, atmosphere="jacchia_roberts", cd=2.2,
    area_mass_ratio_m2_per_kg=0.02,
    low_altitude_model="nrlmsise00", blending_range_m=30_000,
    f107=150, f107_average=150, kp=3,
    use_apparent_sun_position=True, use_approximate_altitude=True,
)
orbit = of.orbits.Hpop(initial=initial, drag=drag)
# data 必须显式提供覆盖传播区间的 UTC、EOP、引力和太阳星历。
satellite = project.objects.satellite("Satellite", orbit=orbit)
analysis = project.prepare(data=data)
```

独立密度、传播、覆盖和跨接口验证见[阻力协议](../validation/atmospheric_drag_protocol.md)。现有状态报告直接查询同一阻力 Session；密度/分项加速度专用报告尚未实现。

## 共享卫星质量

`HpopOptions.satellite_mass_kg` / `HpopSettings.satellite_mass_kg` 默认为 1000 kg，必须有限且正。“其他”页支持 kg、g、t；阻力和光压页的面积质量比仍是独立输入，并显示只读的对应面积。C++ `drag_area_m2()` / `srp_area_m2()`（Python 同名只读属性）返回各自 A/m 与质量的乘积，拒绝溢出或非零乘积下溢为零；零 A/m 允许零面积。

修改质量只改变对应面积，球形加速度、轨迹与计算身份保持不变；设置仍写入项目并标记为已修改。未来非球形或插件模型使用质量时，必须把实际使用的质量纳入身份。此行为依据 [STK Satellite mass](https://help.agi.com/stk/Content/hpop/hpopAdditional.htm)，独立面积/质量模式不作为球形模型的第二套输入。
