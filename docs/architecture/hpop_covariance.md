# HPOP 联合协方差与考虑参数

`propagate_hpop_covariance` 在单个滑行段内传播初始状态协方差，并可考虑常参数 `D=Cd×DragArea/Mass`、`K=Cr×SRPArea/Mass` 的不确定度。C++、Python、安装 SDK 共用 Core 实现。输入／输出为地心 GCRF Cartesian、TAI 和 SI；项目持久化、桌面矩阵编辑、J2000/TOD/LVLH/Frenet 与 FG 表示、科学插值及跨机动传播仍需继续接入，不能把本接口当作完整协方差页面。

参数含义来自 [STK HPOP Covariance](https://help.agi.com/stk/Content/hpop/hpop-covariance.htm)。Value 是方差，不是标准差；交叉值是有单位的协方差，不是无量纲相关系数。未选择的参数用空 optional 表示；没有考虑参数时只传播 6×6 状态矩阵。选定 D 或 K 时增加对应维度，两者均选定时允许 `Cov(D,K)`。现有力被关闭时，其参数敏感度为零；参数本身的方差仍按常参数保留。

## 矩阵、单位与演化

`HpopCovarianceInput.state` 的顺序为 `(rx,ry,rz,vx,vy,vz)`。位置、混合、速度块的单位为 m²、m²/s、m²/s²；参数方差及 D/K 互协方差为 m⁴/kg²；参数与位置／速度的交叉项为 m³/kg、m³/(kg·s)。结果中的 `covariance` 与输入结构相同，历元由 `variational.state.epoch` 给出，可以作为下一滑行段的初始矩阵。

```text
P0 = [Pzz  Pzw; Pwz  Pww],   F = [Φ  S; 0  I],   S = [SD SK]
P(t) = F P0 Fᵀ
Pzz(t) = Φ Pzz Φᵀ + S Pww Sᵀ + Φ Pzw Sᵀ + S Pwz Φᵀ
Pwz(t) = Pwz Φᵀ + Pww Sᵀ,   Pww(t) = Pww
```

只包含实际选择的参数列。输出分别保留初始状态项、参数项、状态与参数交叉项，便于后续报告解释来源；交叉项可以有负特征值。总矩阵独立通过 `F L` 的 Gram 矩阵计算，`L Lᵀ=P0`，减少大正负贡献抵消的损失；不能用截断负数代替公式。分项浮点求和与总结果允许舍入差。参数方差及 D/K 互协方差逐位保留。

## 校验与数值政策

默认零矩阵无效；调用方必须明确提供不确定度。活动的 6、7 或 8 维联合矩阵必须有限、对称且正定，仅检查各对角或各二元相关性不足以保证联合正定。STK 要求整体正定，因此活动零方差会被拒绝；已知常参数应不选择。此规则不妨碍未来编辑器保存未启用草稿。

Cholesky 前以各维标准差缩放为相关矩阵，避免米、米每秒与小参数单位决定校验阈值。只容许相关尺度下 32 个机器 epsilon 的非对称舍入，并取两侧平均；超过则拒绝。正定失败不添加抖动、不裁剪负特征值、不自动取消参数。没有固定的物理单位下限，也不人为限制条件数；Cholesky 不能在双精度下取得正对角时明确失败。

乘法使用缩放点积、补偿求和及指数分解，避免中间的平方或尺度乘积提前上溢／下溢。输出保持显式对称，再校验舍入后的完整活动矩阵；若有限范围或正定性已经无法表达，抛出 `PropagationError`，不发布貌似可用的结果。

## 传播与接口边界

传播复用[变分方程](hpop_variational.md)，因此力模型、独立引力截断、五种积分器、VOP/Sundman、天气与双锥食影边界相同。没有过程噪声、滤波更新或机动执行误差。圆柱阴影和零宽度大气切换仍因未实现 saltation 而拒绝；局部偏导的边界限制继续适用。

`validate_hpop_covariance` 可在开始昂贵传播前校验输入。`transport_hpop_covariance` 可将已经求得的 STM / D/K 敏感度应用到多个初始矩阵；调用方负责将其与正确的初始历元、状态及模型关联。返回值拥有独立数据，输入不被修改；取消和异常不返回部分结果。Python 在释放 GIL 前复制输入，嵌套结果按值读取。

```python
p0 = orbitforge.HpopCovarianceInput()
p0.state = [[(10000.0 if i < 3 else 1.0) if i == j else 0.0
             for j in range(6)] for i in range(6)]
k = orbitforge.HpopConsiderParameter()
k.variance = 1e-4  # (m²/kg)²
p0.srp = k
result = orbitforge.propagate_hpop_covariance(orbit_input, end, p0, data=data)
p_end = result.covariance.state
```

独立参考、矩阵尺度与验收见[联合协方差验证](../validation/hpop_covariance_protocol.md)。
