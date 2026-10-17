# HPOP 状态转移与常参数敏感度

`propagate_hpop_variational` 从初始状态传播一个滑行段，返回末端名义状态、6×6 状态转移矩阵 Φ，以及 Drag / SRP 的六维参数敏感度。C++、安装 SDK 和 Python 使用同一 Core 实现。接口仍采用地心 GCRF、TAI `Epoch` 和 SI；协方差矩阵输入、参考系转换、考虑参数方差、交叉协方差、报告及桌面配置另行接入。项目结构保持 v40 / OBJS31。

参数遵循 [STK HPOP Covariance](https://help.agi.com/stk/Content/hpop/hpop-covariance.htm)：D=Cd×DragArea/Mass、K=Cr×SRPArea/Mass，单位 m²/kg。返回的是对 D/K 的偏导，而非对 Cd/Cr 的偏导。名义参数为零仍保留对应敏感度；关闭某项力则其参数列为零。

## 变分方程与单位

令 x=(r,v)，共享总力给出 Ar=∂a/∂r、Av=∂a/∂v、aD、aK：

```text
A = [0  I; Ar  Av]
dΦ/dt = A Φ,             Φ(t0)=I
dSD/dt = A SD + [0;aD],  SD(t0)=0
dSK/dt = A SK + [0;aK],  SK(t0)=0
```

积分状态包括名义 6 维和附加 6×8 维。Φ 的四个位置／速度块单位为 1、s、1/s、1；参数列的位置／速度单位为 kg/m、kg/(m·s)。内部按位置 1e6 m、速度 1e3 m/s、参数 1 m²/kg 缩放，返回时恢复 SI。附加状态的默认绝对局部容限为 1e−14，允许 1e−16 至 1；相对容限复用 `HpopOptions`。这是明确的数值尺度，不声称是 STK 的误差控制器。

五种积分器均使用已有共用内核。RKF78 / Bulirsch–Stoer 的 VOP 只映射前六个名义变量，附加状态保持物理变分变量。Sundman 坐标下，携带的是 **固定物理时刻的 Φ(t(q)) 和 S(t(q))**，故右端乘 `dt/dq`；不是对 q 轨迹或其初始归一化常数求导。物理端点桥接同时积分这些变量，无需将固定 q 的导数投影成固定 t 的导数。Gauss–Jackson 使用同一组历史系数积分附加一阶方程；Pseudo Correction 仍刷新变分右端，不能冻结修正前的 AΦ。

## 独立引力截断

`HpopVariationalOptions.gravity` 为空时使用名义阶／次；也可显式给出 `HpopGravityTruncation(degree, order)`。验证模型实际阶次、长期项及潮汐依赖，最高范围与原引力模型相同。名义加速度始终使用 `HpopOptions`，只对变分空间块使用独立截断。固体潮的截断开关相对于所选变分阶次解释，海潮保留其自身独立截断。

使用不同阶次时，Φ 和参数敏感度是所选协方差动力学的近似，不能再称为完整名义高阶轨迹的精确 Jacobian。Python 数据装配取两套引力需求的并集，避免名义 0×0 而变分非中心场时遗漏日月潮汐资源。该选择不改变名义力模型；自适应变分误差可能改变接受步长。

## 覆盖、边界与失败

一个调用只处理 `[初始历元, 末端历元]` 内的正向或反向滑行，最长 366 天；没有区间外力评估、插值或跨机动传播。资源为不可变快照，没有隐式联网或外推。沿用最低高度、有限性、200000 次尝试上限与取消检查，失败不返回部分状态。

空间天气是外部固定时刻的跳变：继续使用单侧天气取样并重启积分历史；状态、Φ、S 连续。球面双锥食影的力连续，启用 Boundary Mitigation 时使用既有接触分段及高精度桥接，附加状态随之积分。即使 Cr 或面积质量比为零，也保留 K 敏感度所需的食影边界处理。

当前明确拒绝分配了遮挡体的圆柱阴影及 JR 的零宽度低空密度切换；它们需要状态相关跳变的 saltation 方程。既有[阻力偏导](drag_differential.md)分界／极点和 [SRP 偏导](srp_differential.md)退化几何限制仍适用。没有自动添加过程噪声、参数方差或机动噪声。

## 使用

```python
options = orbitforge.HpopVariationalOptions()
options.gravity = orbitforge.HpopGravityTruncation(4, 4)
result = orbitforge.propagate_hpop_variational(orbit_input, end, options=options, data=data)
phi = result.state_transition
sensitivity_d = result.drag_sensitivity
sensitivity_k = result.srp_sensitivity
```

`orbit_input` 必须为单段 HPOP。结果属性只读，读取矩阵与状态得到独立值；输入与数据在释放 Python GIL 前按值捕获。选项、绑定和 `_native.pyi` 同步导出。独立参考、边界测试和数值界限见[变分传播验证协议](../validation/hpop_variational_protocol.md)。
