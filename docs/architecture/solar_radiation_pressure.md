# 太阳辐射压

HPOP 的球形太阳辐射压由 Core 实现，桌面、C++、Python、报告与保存后的重新分析使用相同计算。它是 [HPOP 对齐清单](hpop_alignment.md)中的一个增量：已支持球形卫星、独立 Cr 与面积质量比、距离平方缩放、无阴影／圆柱／双锥、地球与月球遮挡及地球大气等效高度。

## 输入与计算

所有计算参数使用 SI；位置为同一历元、同一惯性轴系的地心几何位置。令 `s = r_sun − r_sat`，计算

```text
a_srp = −lighting_ratio · P_ref · (d_ref / |s|)² · Cr · (A_srp / m) · s / |s|
```

默认 `P_ref = 4.56e-6 Pa`、`d_ref = 149597870700 m`、`Cr = 1`、`A_srp/m = 0.02 m²/kg`；该面积质量比独立于阻力。太阳、地球、月球球面半径分别为 `695700000`、`6378137`、`1737400 m`。全部常数可显式修改并参与有效输入身份；不将这些默认值当成所有 STK 配置的默认值。[Orekit 13.1.2 光压实现](https://github.com/CS-SI/Orekit/blob/13.1.2/src/main/java/org/orekit/forces/radiation/SolarRadiationPressure.java)

Cr 范围为 `[-100, 100]`，面积质量比为 `[0, 9999] m²/kg`，允许带符号的经验 Cr 与零面积。光压、参考距离和球面半径必须有限且为正，大气高度必须有限且非负，组合半径必须有限。所有设置草稿都须是有效数值；禁用时仍保存有效草稿。启用时卫星位于太阳内部或已分配遮挡球体内部会明确失败。[STK HPOPForce 参数范围](https://help.agi.com/stkEngineOnUNIX/Subsystems/connectCmds/Content/cmd_HPOPForce.htm)

`SrpShadowModel::none` 使用全照；`cylindrical` 按太阳方向射线是否穿过任一分配球体判定 0/1；`dual_cone` 使用均匀亮度的视太阳圆盘，支持全照、半影、本影和环食。双锥在太阳方向的局部角平面上求地球／月球视圆盘并集与太阳圆盘的交面积，以 Green 定理分段圆弧积分求遮挡比例。重叠遮挡只计一次，不能把两个独立照明比例相乘。地球等效半径为球面半径加大气高度；月球不加该高度。

当前遮挡体为球形地球和月球，太阳位置为几何位置。STK 的地球椭球轮廓、视太阳位置及光行时、Boundary Mitigation、其他可选遮挡体仍待实现。圆柱存在不连续边界，双锥在边缘不保证高阶光滑；当前按每个 RK 阶段求值，尚无专用食边界事件定位，使用者应通过步长与容差收敛检查评估穿越误差。[STK SRP 设置](https://help.agi.com/stk/Content/hpop/hpopSRP.htm)

## 资源、来源与持久化

光压启用时总是需要太阳星历，启用阴影且分配月球时还需要月球。第三体引力勾选状态与此独立，GM 不参与光压力值。每个积分阶段使用对应历元的 Provider；准备阶段检查完整传播区间与实际目标链。缺文件或覆盖不足会停止准备，不能自动关闭光压。几何位置经现有 SPICE 力模型适配器进入 GCRF；SDK 不隐式下载资源。

会话和分析持有不可变星历；来源包含 `spherical-geometric-uniform-angular-union-v1`、有效光压参数及星历解释／内容身份。禁用光压的草稿，以及无阴影或未分配遮挡体的半径草稿不改变分析和报告身份。项目 v20／OBJS11 保存全部有效草稿；不提供旧版本解码。

```python
data = of.DataSnapshot.builtin_time().with_ephemeris("de440s", manifest, workspace)
orbit = of.orbits.Hpop(
    initial=initial, gravity_degree=0,
    srp=of.orbits.SrpSettings(enabled=True, cr=1.2,
                             area_mass_ratio_m2_per_kg=0.02,
                             shadow_model="dual_cone", earth_shadow=True, moon_shadow=True))
sat = project.objects.satellite("SRP", orbit=orbit)
analysis = project.prepare(data=data)
trajectory = analysis.states(sat, step=of.units.s(60))
```

C++ 使用 `HpopOptions::srp`／`SrpOptions`，通过显式 `HpopEnvironment` 或 `Project::prepare(data)` 装配资源。低层 `evaluate_srp` 返回加速度与照明比例，便于独立几何验证；它不加载文件、不查询 Provider、不修改输入。Python 提供对应原生 `SrpOptions`、`SrpShadowModel`、`SrpEvaluation`、`evaluate_srp`，易用接口使用不可变 `of.orbits.SrpSettings`。

独立 Orekit 轨迹参考、独立面积积分和边界测试见 [HPOP 验证协议](../validation/hpop_protocol.md)。这些验证不代表同配置 STK 的直接轨迹对照或实际预报精度。
