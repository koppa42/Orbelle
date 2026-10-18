# 卫星视太阳与 SRP 状态偏导

`ApparentSunDifferential` / `apparent_sun_from_spacecraft_differential` 返回固定接收历元的卫星至太阳视向量及其对卫星地心状态的两个 3×3 Jacobian。C++ Provider、Runtime 适配器和 Python 使用同一实现；Python 结果只读且返回自有副本。单位分别为 m、无量纲及 s，矩阵行为输出分量、列为输入分量。输入位置为 m，速度为每 TAI 秒的 m/s，惯性轴沿用 HPOP 的 GCRF / ICRF 同轴牛顿模型约定。

HPOP 的 True、Apparent To True CB、Apparent 对应 [STK SRP 太阳位置设置](https://help.agi.com/stk/Content/hpop/hpopSRP.htm)。当前名义视位置继续采用 NAIF 接收型单次 LT 加光行差；STK 帮助的选项描述不足以证明两者内部实现完全相同。

## 解析模型

设接收 TDB 时刻为 t，地球与太阳的 SSB 位置为 E、S，卫星地心输入为 r、v，`ρ=dTAI/dTDB`：

```text
O = E(t) + r                     观测者 SSB 位置
V = V_E(t) + ρv                 观测者每 TDB 秒的 SSB 速度
p0 = S(t) − O,   n0 = p0/|p0|
t_e = t − |p0|/c                一次 LT 发光时刻
p = S(t_e) − O,   d = |p|,   n = p/d
β = V/c,   b = β·n,   q = β − bn,   γ = sqrt(1 − q·q)
u = γn + q
p_app = d u
```

光行差公式来自 [NAIF stelab](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/cspice/stelab_c.html) 的旋转定义。生产名义值仍直接调用 `stelab_c`；解析式只用于偏导。固定 t 时地球状态与 ρ 不依赖卫星状态：

```text
B = (V_S(t_e)/c) n0ᵀ − I
P = I − nnᵀ
Jp = u nᵀ + [(γ−b) I + n (bq/γ−β)ᵀ] P
∂p_app/∂r = Jp B
∂p_app/∂v = d ρ/c [I − n (n+q/γ)ᵀ]
```

发光太阳速度来自同一已检查历元的几何 SPK 状态。公式对应单次 LT，未改成收敛光行时 CN，也不含广义相对论光传播。没有以 `|n×β|` 为分母；SSB 零速度和沿视线运动处仍保留横向速度敏感度。

## SRP 完整局部状态链

C++ / SDK `srp_state_differential(state, options, ephemeris)` 与 HPOP 共用太阳方法和遮挡体几何解析。地心坐标的所选太阳为 `S_selected=r+p_app`。令低层 [SRP 偏导](srp_differential.md) 的固定太阳状态块为 Jr、Jv，太阳块为 Js：

```text
Jr_full = Jr + Js (I + ∂p_app/∂r)
Jv_full = Js ∂p_app/∂v
```

True 与地心视太阳在固定历元与卫星状态独立，直接复用低层状态块；`K=Cr×A/m` 偏导保持独立。遮挡体仍是同接收历元的几何中心，此次没有引入遮挡体视位置或椭球轮廓。返回 `nominal`、完整局部 `ForceDifferential` 及 K 偏导，不积分 STM、consider 参数或协方差。

## 边界与生命周期

- 原有名义视向量与新增结果的 `position_m` 使用同一求值路径。普通轨迹不求导，也不额外查询发光太阳速度。CSPICE 批次仍持有全局锁并激活不可变快照；Python 调用释放 GIL。
- 查询发光状态前先核对清单窗口；实际 SPK 缺口由查询失败报告。输入非有限、SSB 速度达到光速、无效视线或非有限偏导明确失败。新增接口沿用 Provider 错误类型，失败后合法查询仍可复用。
- 不支持视太阳偏导的第三方 Provider 明确拒绝。启用的完整 SRP 状态入口要求地心 GCRF 和有限状态；禁用时校验选项后返回零，不检查状态或查询星历。
- [阴影接触和重合圆盘限制](srp_differential.md#阴影面积变化)仍适用。名义 Boundary Mitigation 不代替变分方程的事件处理；完整 STM 和协方差接口仍待实现。
- 项目没有新增设置，继续使用当前 v41 / OBJS32。此计算接口尚未接入 Python 的完整力偏导／协方差 API；Python 已导出星历层结果和 Provider 方法。

独立 CSPICE 差分、合成时标和安装后接口验证见[验证协议](../validation/apparent_sun_differential_protocol.md)。
