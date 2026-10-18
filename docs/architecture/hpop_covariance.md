# HPOP 联合协方差与考虑参数

`propagate_hpop_covariance` 在单个滑行段内传播初始状态协方差，并可考虑常参数 `D=Cd×DragArea/Mass`、`K=Cr×SRPArea/Mass` 的不确定度。C++、Python、安装 SDK 共用 Core 实现。输入支持带标记的 GCRF / LVLH / Frenet / FK5 J2000 / TrueOfDate Cartesian，输出统一地心 GCRF，时间与单位为 TAI 和 SI；项目 v44 已保存完整协方差草稿；桌面 HPOP → 协方差提供初始矩阵、参考系、独立重力截断及 D/K 考虑参数编辑。项目 Analysis 与协方差报告共用 Core 传播。JPLFile 章动模式、FG 表示、科学插值、跨机动传播与更多考虑参数列入[延期范围](hpop_alignment.md#延期范围)，不作为首版完成条件。

参数含义来自 [STK HPOP Covariance](https://help.agi.com/stk/Content/hpop/hpop-covariance.htm)。Value 是方差，不是标准差；交叉值是有单位的协方差，不是无量纲相关系数。未选择的参数用空 optional 表示；没有考虑参数时只传播 6×6 状态矩阵。选定 D 或 K 时增加对应维度，两者均选定时允许 `Cov(D,K)`。现有力被关闭时，其参数敏感度为零；参数本身的方差仍按常参数保留。

## 矩阵、单位与演化

`HpopCovarianceInput.coordinates` 同时标明轴系和速度约定；`state` 的顺序为 `(rx,ry,rz,vx,vy,vz)`。位置、混合、速度块的单位为 m²、m²/s、m²/s²；参数方差及 D/K 互协方差为 m⁴/kg²；参数与位置／速度的交叉项为 m³/kg、m³/(kg·s)。结果中的 `covariance` 与输入结构相同，历元由 `variational.state.epoch` 给出，可以作为下一滑行段的初始矩阵。

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

## 局部参考系与速度约定

`HpopCovarianceCoordinates` 由 `frame` 和 `velocity` 两个枚举组成。默认 GCRF / projected；这只是本 API 的明确默认值，不声称它是 STK 初始协方差编辑器的默认值。当前支持下列轴系：

| `HpopCovarianceFrame` | 轴顺序 |
| --- | --- |
| `gcrf` | 地心 GCRF |
| `lvlh` | STK LVLH：R=r/|r|，C=(r×v)/|r×v|，I=C×R；轴为 (R,I,C) |
| `frenet` | STK Frenet：T=v/|v|，N=T×C；轴为 (N,T,C) |
| `fk5_j2000` | 通过 pseudo-fixed 实现的 FK5 J2000，IERS1996 章动 |
| `true_of_date` | 同一 FK5 链的真赤道、真春分点 of date |

遵循 [STK 参考系](https://help.agi.com/stk/Content/stk/referenceframesvgt.htm)和 [HPOP Frenet 定义](https://help.agi.com/stk/Content/hpop/hpop-covariance.htm)。STK LVLH 与 CCSDS LVLH / STK VVLH 的轴排列不同；这里不混用同名定义。

STK 分别提供 [Projected](https://help.agi.com/stk/Subsystems/dataProviders/Content/html/dataProviders/Pos_Vel_Projected_Covariance.htm) 和 [Rotated](https://help.agi.com/stkEngineOnUNIX/Subsystems/dataProviders/Content/html/dataProviders/Pos_Vel_Rotated_Covariance.htm) 协方差报告。对应 `projected` 为惯性位置／速度误差在新轴上的分量，`rotating` 的速度则为相对转动轴的时间导数：

```text
J_projected = [R 0; 0 R]
J_rotating  = [R 0; Rdot R]
J_source_to_target = J_target × inverse(J_source)
Pzz' = J Pzz Jᵀ;   Pwz' = Pwz Jᵀ;   Pww' = Pww
```

这些公开定义不能用来推定 STK HPOP 初始矩阵页未说明的速度约定；对应初始化设置仍待实机验收。API 将两种语义显式区分，不能因页面都显示 LVLH 就省略旋转项。

`hpop_covariance_frame_jacobian` 以同历元的 Earth/GCRF 名义位置、惯性速度构造 6×6 Jacobian。局部轴固定于名义轨迹，不随每个误差样本重新构造。转动模式还要求实际名义惯性加速度（m/s²），解析求各轴时间导数，包括横向加速度导致的轨道面转动；显式零加速度表示直线运动，不代入二体加速度。投影模式不要求加速度。

`convert_hpop_covariance` 变换完整活动联合矩阵，复用缩放 Cholesky 与平方根乘法，D/K 方差及互协方差逐位保持，所有参数／状态交叉项同步转换。调用方负责将矩阵与传入的名义状态／历元关联。源、目标分别携带约定，支持局部系互转、逆变换和同轴下的 projected/rotating 转换。返回结果携带目标标记。

`propagate_hpop_covariance` 接受上述局部输入，在初始时刻转入 GCRF；转动模式使用完整名义 HPOP 力求得的瞬时加速度，独立变分引力截断不改变它。初始事件点沿用 `evaluate_hpop_acceleration` 的时间取样，不自动选择传播方向的单侧极限。需要指定单侧定义时，先用该侧加速度显式转换。传播结果、STM、敏感度及三个贡献项均为 GCRF；可在末端用相应名义加速度重新转换结果矩阵。

非法参考系、非有限状态／加速度、局部系的零 r/v 或 `|rhat×vhat| <= 64 ε` 会被拒绝；这个门槛只排除双精度无法分辨的径向退化，不设置轨道倾角／偏心率限制。矩阵转换不推进时间；局部轴不增加数据资源。J2000 / TrueOfDate 使用独立的天文转换，不能把 GCRF 或 SPICE J2000 改名替代。

天文系通过 `HpopCovarianceFrameEnvironment` 注入不可变 Frame/EOP Provider；Python 从 `data=DataSnapshot` 捕获同样资源。一次转换只读取初始历元的一次 EOP，派生速度使用该点局部变化率，不在覆盖范围外查询。缺少数据、历元越界或 Provider 不支持时明确失败。同系转换也验证所需数据。`propagate_hpop_covariance` 复用力环境的 Provider，在积分前转入 GCRF；天文系不需要轨道加速度。

实现遵循 STK 的两条 pseudo-fixed 路径：现代 GCRF 使用 IAU2006/2000A、ERA 与 dX/dY，FK5 使用 IERS1996 的 IAU1980 章动、Lieske 岁差、GMST82 及分点方程。极移在这个转换中不参与；J2000 差异随时间变化。返回的 `PositionRotation.model` 保留模型标识。这是 STK 支持的 **IERS1996 模式**；STK 默认 JPLFile/DE405 章动及实机逐位比较仍待实现/验收。数据和边界见[天文系验证协议](../validation/hpop_celestial_frames_protocol.md)。

参考生成、200 组成熟算法对照与误差门槛见[局部系验证协议](../validation/hpop_covariance_frames_protocol.md)。

## 传播与接口边界

传播复用[变分方程](hpop_variational.md)，因此力模型、独立引力截断、五种积分器、VOP/Sundman、天气与双锥食影边界相同。没有过程噪声、滤波更新或机动执行误差。圆柱阴影和零宽度大气切换仍因未实现 saltation 而拒绝；局部偏导的边界限制继续适用。

`validate_hpop_covariance` 可在开始昂贵传播前校验输入。`transport_hpop_covariance` 可将已经求得的 GCRF STM / D/K 敏感度应用到多个 GCRF 初始矩阵；其他帧必须先显式转换，否则拒绝；调用方负责将其与正确的初始历元、状态及模型关联。返回值拥有独立数据，输入不被修改；取消和异常不返回部分结果。Python 在释放 GIL 前复制输入，嵌套结果按值读取。

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


## 项目与桌面

`OrbitInput.covariance` 使用 `application::HpopCovarianceSettings`。输入矩阵对应轨道初始历元；无默认不确定度，零矩阵只可作为禁用草稿。编辑下三角自动镜像上三角，切换参考系重新解释矩阵，不偷偷变换数值。桌面明确选择惯性速度分量投影或相对转动坐标系速度，不猜测 STK 初始矩阵页未公开的默认约定。J2000/TOD 使用 IERS1996。

所有草稿都须有限，矩阵对称、对角非负；启用时检查整个活动联合矩阵正定。考虑分析、D/K 使用标记与参数间交叉开关分别保存；非活动草稿不进入分析/报告输入哈希。独立重力截断所激活的潮汐、UTC/EOP、星历资源参与准备与身份，不能仅检查名义 0×0 力模型。项目准备在初始点预检模型、偏导和坐标资源，失败不发布候选项目。任务序列启用协方差目前明确拒绝。

`Analysis::covariance(satellite, at)` / Python `analysis.covariance(satellite, at=epoch)` 从不可变项目及数据快照读取配置，按精确 TAI 端点积分，返回完整联合协方差、STM 和分项结果；支持取消，不返回部分结果。旧快照不受后续编辑影响。

“位置速度协方差”报告（`ReportKind::state_covariance`）仅选择已启用协方差的 HPOP 卫星，输出 GCRF 矩阵下三角的 21 个元素，可使用已有表格、曲线、模板与 CSV。SI 单位是 m²、m²/s、m²/s²；工程单位使用 km²、km²/s、km²/s²，均乘 10⁻⁶。每个报告点目前从初始历元独立积分，不插值，长时段密集输出仍需后续优化。报告保存配置及来源，不把显示缓存当作数值输入。

验证见[项目协方差接入协议](../validation/hpop_covariance_project_protocol.md)。
