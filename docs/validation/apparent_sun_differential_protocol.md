# 卫星视太阳与 SRP 状态链验证

验证 [ApparentSunDifferential 和完整局部 SRP 状态链](../architecture/apparent_sun_differential.md)，不将通过局部导数验收表述为完整协方差或 STK 轨迹验收。

## 独立参考与输入

`modules/providers/spice/tests/provider_test.cpp` 直接调用未修改的 CSPICE N0067 `spkssb_c` / [`spkapo_c(...,"LT+S")`](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/cspice/spkapo_c.html)，独立于生产显式发光历元查询及 `stelab_c` 路径。参考不调用新增解析偏导。测试串行使用已激活的快照；生产仍通过封装锁管理全部 CSPICE 调用。

- 81 组一般状态：54 组合成线性 Sun/Earth Type-2 SPK（两种 LSK、三个历元、三个位置、三个速度符号），以及 27 组固定 DE440s 样例（窗口 10% / 50% / 90% 的三个历元、相同位置与速度组合）。位置覆盖 LEO、GEO 附近及远距离观测者。
- 合成时标取 `DELTET/K=5`、`EB=0.1`、`M=(0.4,0.002)`，放大 TAI/TDB 换算。参考以独立写明的 LSK 常数和 long double 表达式计算 `dTAI/dTDB`。它用于辨别时标错误，不代表真实时间模型。
- 另有 3 组 SSB 速度为零或沿视线 ±0.5c 的边界输入；名义结果与仅 LT 的射线一致，但横向速度偏导仍非零。高速度仅检查 NAIF 公式的数学行为，不能据此扩展 HPOP 的物理适用域。
- 原始真实星历、LSK 和散列沿用 `data/samples/spice/de440s_2016_leap/manifest.json`；线性系数、状态、时标及固定输入直接保存在测试源码，测试期间生成隔离临时文件。

各笛卡尔分量使用八阶中心差分，正侧权重为 `4/5, −1/5, 4/105, −1/280`，负侧取反。位置步长为 50000 / 100000 m，速度步长为 128 / 256 m/s。较大步长用于避免约 1 AU 向量的双精度消减；八阶截断误差通过两档步长检查。以每块最大绝对分量 s 为尺度，参考两步长每分量差须不大于 `3e−9 s+1e−10`，解析值误差不大于 `5e−9 s+1e−10`，分别使用该块单位。名义向量与独立 CSPICE 相差不超过 1 mm，与原 Provider 入口逐位相同。

当前 MSVC 最大 `|解析−参考|/s`：位置块 `6.653e−10`，速度块 `5.019e−10`。这些是固定输入的数值一致性，不是实际星历精度。

## 完整 SRP 状态链

48 组输入包括三种太阳方法、LEO / GEO 两种轨道半径、四种阴影边缘偏移以及两种速度。普通速度为 `(2000,6500,−1000)` m/s；合成速度 `(4e7,3e7,−2e7)` m/s 用于放大位置链，检测漏乘 `I+∂p_app/∂r`。该速度不作为物理轨道参考。Apparent 的几何构造以相应视太阳方向为轴，包含日照、半影和本影。

参考太阳来自原始 CSPICE，名义力使用已有 `evaluate_srp`，再分别对 r、v 作八阶中心差分，步长为 128 / 256 m 或 m/s。参考两步长差的逐分量限为 `5e−7 s+2e−23`，实际解析误差限为 `1e−6 s+2e−23`。这里 s 是该参考块最大绝对分量，绝对底限使用对应 Jacobian 单位。统一小步长避免参考差分跨越阴影分支；没有通过放宽容限处理跨分支误差。

这是对星历到力的链式装配验证；它复用了名义光压函数，不能单独证明阴影面积算法正确。面积、力和局部偏导另有 [641 组 Orekit、mpmath、SciPy 独立参考](srp_differential_protocol.md)。当前完整力位置／速度块最大归一化误差为 `3.959e−7` / `2.353e−8`，报告尺度为 `s+2e−17`。放大案例中完整位置块与固定太阳块之差达到块尺度的 `0.1548`；测试要求该量大于 `1e−5`，保证遗漏位置链可被现有误差限辨别。

## 边界、运行和交付

`Spice.ApparentSunDerivativeFailuresPreserveUsableProvider` 验证 NaN、超光速和发光历元越界拒绝，失败后结果逐位恢复。`SrpStateDifferential.*` 验证固定太阳方法的精确装配、默认 Provider 拒绝、无效参考系和非有限速度、禁用零结果及不查询星历。

按 [构建说明](../development/build.md) 配置 `ORBELLE_WITH_SPICE=ON`、测试和必要的时间参考系依赖，执行构建目录内的测试程序：

```text
orbelle_spice_tests --gtest_filter=Spice.*Apparent*:Spice.SrpStateChain*
orbelle_core_tests --gtest_filter=SrpDifferential.*:SrpStateDifferential.*:SolarRadiationPressure.*:SrpBoundary.*
```

测试 XML 记录导数误差和位置链可辨识量。相关回归保留 HPOP 的三种太阳方法逐阶段输入、第三体独立几何查询、SRP 项目与运行时集成。Python 在隔离安装环境检查返回类型、单位、只读副本、失败恢复及两个不可变快照的并发切换；SDK 消费端实际调用 Provider 和 Runtime 的完整 SRP 状态接口。执行日志、构建配置和安装结果存入新的 `out/validation/` 目录。当前没有 STK 同版本同配置的实测数据。
