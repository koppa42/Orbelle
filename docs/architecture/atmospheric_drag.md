# HPOP 大气阻力

桌面阻力页、C++ `DragOptions` 和 Python `orbits.DragSettings` 共用 Core 模型。当前实现球形阻力、Jacchia–Roberts / NRLMSISE-00 密度、手动空间天气、低空拼接及地心视太阳位置；低层密度接口支持原生日 Ap 和七项历史 Ap；HPOP 天气文件、历史窗口装配、风场和近似高度尚未实现。[STK 阻力设置](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)是功能与输入含义的对齐依据；尚未取得 STK 同配置数值输出。

## 力与参考系

在 ITRF 中计算 `a = -0.5 * rho * Cd * (A/m) * |v_rel| * v_rel`，单位为 kg/m³、m²/kg、m/s、m/s²。大气与地球共转且无风；`v_rel` 来自完整 GCRF→ITRF 状态变换，包含旋转率项。只旋转惯性速度会错误地把地球自转计入相对速度。每个积分阶段重新求 UTC、EOP、太阳和密度，再将加速度旋回 GCRF。

高度固定使用精确 WGS84 椭球高，半长轴 6378137 m、扁率 1/298.257223563；近极点有有限处理。太阳赤纬与当地太阳时由同一 ITRF 太阳矢量求得，太阳时角是向东相对太阳子午线的夹角。地心奇点、非有限输入和域外高度明确报错。

Cd 为带符号经验系数，范围 −100…100；面积质量比为 0…9999 m²/kg，与光压参数独立。这两项范围对应 [STK HPOP Drag 命令](https://help.agi.com/stk/12.10.0/Subsystems/connectCmds/Content/cmd_HPOPDrag.htm)。负 Cd 产生反向经验加速度；正常阻力使用正 Cd。零面积或零 Cd 不改变已启用模型的数据依赖。

## 密度与天气

Jacchia–Roberts 适用 90…2500 km，保留 NASA GMAT 标量模型的拟合常数及分段公式，并处理正午、午夜、极点、太阳赤纬为零的有限极限。GMAT 外层 `Density` 不开放 90…100 km，本实现使用其原有标量公式开放这一高度段。NRLMSISE-00 适用 0…1000 km，移植 Orekit 13.1.2 的双精度计算和表格，使用 `gtd7d` 总质量密度，包含异常氧；默认变化项开启，使用日 Ap。源码来源和 Apache-2.0 许可见 [模型说明](../../modules/core/src/propagation/atmosphere/README.md)。所有临时状态属于单次调用，模型可并发使用。

当前天气来源为常量手动输入：日 F10.7、平均 F10.7（sfu）、Kp。NRLMSISE 将手动日值作为前一日 F10.7，将平均值作为模型平均通量；没有隐含的历史查询。Kp 使用标准 1/3 刻度的 28 项 Ap 表，非表格刻度线性插值，Kp=3 对应 Ap=15。界面/API 当前限制通量 `(0,1000]` sfu、Kp `[0,9]`，这些限制与 STK 命令允许的全部范围不同，不能据此宣称所有参数范围已对齐。低层 `AtmosphereInput` 使用 UTC 公历日期 1600…2600；闰秒合法性在 HPOP 中由实际时间 Provider 认证，独立标量调用方负责保证其 UTC 真实有效。

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

这个入口用于独立密度计算及后续空间天气 Provider 装配；调用方须提供与查询 UTC 对应的完整窗口，接口不读取文件、不补齐缺值。当前 `DragSettings`、`evaluate_drag` 和 HPOP 传播仍使用手动常量 Kp，尚未把历史 Ap 接入轨道传播或桌面文件选择；项目格式仍为 v19。本增量不宣称完成 STK 的天气文件模式。

## 数据与持久化

阻力即使只启用点质量引力，也需要覆盖完整传播路径的 UTC、EOP、Frame Provider 和太阳星历。启用后不因缺项而退回无阻力；UTC 内容哈希必须与 EOP 保留的时间来源一致。数据快照保留这些 Provider 的共享所有权，编辑项目或更新数据不改变已准备的计算。

默认使用几何太阳。`use_apparent_sun_position=True` 使用 CSPICE 对地球接收观测的 `LT+S` 太阳方向；此约定影响密度中的当地太阳时/赤纬，不修改第三体引力或光压的几何太阳。准备计算时额外认证区间起点前 600 s 的太阳与地球核覆盖，边界不足明确拒绝。自定义星历必须提供相应查询和区间认证接口，不能将几何方向冒充视方向。

项目格式为 v19 / OBJS v10，只读写当前版本。禁用模型时保存合法参数草稿，计算身份忽略这些草稿；启用时计入实际密度模型、天气、Cd、面积质量比、太阳方法及 UTC/EOP/星历来源。NRLMSISE 主模型的低空草稿和未启用的拼接范围不改变结果身份。

```python
import orbitforge as of

drag = of.orbits.DragSettings(
    enabled=True, atmosphere="jacchia_roberts", cd=2.2,
    area_mass_ratio_m2_per_kg=0.02,
    low_altitude_model="nrlmsise00", blending_range_m=30_000,
    f107=150, f107_average=150, kp=3,
    use_apparent_sun_position=True,
)
orbit = of.orbits.Hpop(initial=initial, drag=drag)
# data 必须显式提供覆盖传播区间的 UTC、EOP、引力和太阳星历。
satellite = project.objects.satellite("Satellite", orbit=orbit)
analysis = project.prepare(data=data)
```

独立密度、传播、覆盖和跨接口验证见[阻力协议](../validation/atmospheric_drag_protocol.md)。现有状态报告直接查询同一阻力 Session；密度/分项加速度专用报告尚未实现。
