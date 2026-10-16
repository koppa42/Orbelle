# HPOP 时间正则化

本页记录内部数值内核；公共 `HpopOptions`、Python、项目格式和桌面尚未开放时间正则化。真实 TAI 端点、天气跳变、机动、固定输出和自适应步长须完成验证后再接入配置。当前 HPOP 的公开计算仍使用普通时间。

## STK 语义与内部尺度

[STK Integrator](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm) 与 [Connect 命令](https://help.agi.com/stk/SubSystems/connectCmds/Content/cmd_HPOPIntegrator.htm) 定义 `dS = sqrt(mu) r^(-n) dt`，以 Exponent 和 Steps Per Orbit 设置正则化步长；二体问题中 n=1 对应偏近点角、n=2 对应真近点角。[官方比较](https://help.agi.com/stk/content/hpop/example_HPOPResults.htm) 包含 Gauss–Jackson 与时间正则化的组合。当前实现依据这些公开方程，不推定 STK 的内部控制器与所有合法范围。

`SundmanTransform` 取初始密切椭圆的半长轴 a0 和偏心率 e0，使用常数尺度 `q = S / a0^(3/2-n)`：

```text
T0 = sqrt(a0^3 / mu)
g(r) = dt/dq = T0 (r/a0)^n
Q0 = integral[0,2pi] (1-e0 cos(E))^(1-n) dE
fixed dq = Q0 / steps_per_orbit
```

n=1 时 Q0=2π；n=2 时 Q0=2π/sqrt(1-e0²)。其余指数以误差受控的自适应 Simpson 求积；n>2 使用真近点角的等价积分，避免窄近地点峰值。以 `(1-e)+2e sin²(E/2)` 等形式减少近抛物线输入的消减误差。归一化保持同一个初始轨道尺度，摄动不会在每步重置它。

内部验证范围为有限 `0<n<=10`、正 μ、非径向束缚初始轨道。非椭圆没有可用于 Steps Per Orbit 的周期，当前明确拒绝；不临时替换半长轴。官方 Connect 页面印出的指数不等式存在歧义，此内部范围尚不代表已核实 STK 的完整合法域。

## 方程与共用积分内核

RK4、RKF7(8)、RKV8(9)、Bulirsch–Stoer 使用七维状态 `(r,v,t)`：`r'=g v`、`v'=g a(t,r,v)`、`t'=g`。力评估收到真实秒与物理速度。内部一步积分器按状态维数和分量误差尺度模板化，普通时间 HPOP 复用相同系数与算法；正则化控制器可使用每步局部增量作补偿累加，避免把微小增量反复加到大的位置或时钟上。

Gauss–Jackson 使用混合系统 `(r,u,t)`，其中 `u=dr/dq`：

```text
r'' = g² a(t,r,u/g) + k u
k = n (r dot u) / |r|²
t' = g(r)
```

三维位置使用二次求和，真实时间使用同一方法的一次求和系数。`g(r)` 由位置直接求值；不另积分一个可能漂移的时钟导数，也不在步末投影状态。Pseudo Correction 保留最后一次力评估的物理摄动，加上校正状态的二体项，再更新全部坐标变换因子。Full Correction 重新调用完整力。

固定十八阶 BS 和 GJ 的十四至十八阶起步器使用 2/4/6/8/12/16/24/32/48 子步序列，并外推局部状态增量。该序列改善高阶外推的条件数；普通自适应 BS 保持 2/4/…/18。固定 BS 的算法来源升级为 `fixed18-v2`，GJ 两种校正的起步来源升级为 `startup-*-v2`，项目结构仍为 v35 / OBJS26。

固定步数的 GJ 没有截断误差控制；校正收敛不等于轨道精度合格。高偏心率、尤其 n=2 时须检查步数加倍的结果。内核验收使用固定正则化端点，覆盖 e=0/.7/.98、正反向超过一圈、时变与速度相关摄动；完整范围、参考误差和命令见[验证协议](../validation/time_regularization_protocol.md)。

## 后续接入

实际使用需把指定 TAI 历元求解为正则化步末，保持整数纳秒报告网格；所有阶段须位于已验证的力数据覆盖内。天气跳变保持单侧求值，机动后重建归一化与多步历史，自适应控制包含时钟误差。启用、指数和每圈步数随后贯通 Core、Runtime、Python、项目格式及 Integrator 页面。在这些边界完成前，本内部验收不构成可选的桌面能力，也不构成同配置 STK 实测。
