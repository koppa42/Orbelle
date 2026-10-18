# HPOP 总力与状态块装配验证

[总力入口](../architecture/hpop_force.md)将已经各自验证的力模型装配到同一地心惯性状态。验证区分独立模型结果、坐标／参数链、共用入口的回归；本文只验证局部状态块；全弧 STM 与 D/K 敏感度另见[变分传播验证](hpop_variational_protocol.md)，[联合协方差](hpop_covariance_protocol.md)另行验证；局部及天文帧转换分别由[局部帧协议](hpop_covariance_frames_protocol.md)和[天文帧协议](hpop_celestial_frames_protocol.md)验证，项目、桌面和报告接入见[项目验证协议](hpop_covariance_project_protocol.md)。

## Orekit 独立组合参考

`tools/validation/orekit/OrbitForgeHpopForceReference.java` 调用未修改的 Orekit 13.1.2 力模型与 Hipparchus 4.0.2 `Gradient`，八个独立变量为 r、v、D、K。`NRLMSISE00` / `DragForce` 显式关闭密度有限差分；参考的密度导数为自动微分，生产仍采用既有局部密度梯度方法。

216 组 = 三个旋转 × 两个高度 × 两个纬度 × 三组有符号系数 × 六种力组合。高度 245 / 750 km，纬度 −0.8 / 0.6 rad、经度 1.2 rad，WGS84 椭球；固定接收历元为 2000-01-01 12:00:00 TAI，合成 UTC 与它相同。平滑偏导案例避开 NRLMSISE-00 的 240 km 内部分界，分界行为由专门错误测试保留。

- 地固轴绕 `(1,2,−1)` 旋转 −0.7、0.2、1.1 rad，角速度 `(2e−5,−3e−5,7e−5)` rad/s。参考直接输出 Orekit 变换的 R 与 Rdot，C++ 用同一矩阵定义外部环境，独立检查张量旋转和位置到速度耦合。
- 中心 GM=`3.986004415e14` m³/s²，参考半径 `6378136.3` m，tide-free 2×2 归一化场；C20=`−4.84165371736e−4`，C21/S21=`3e−7/−4e−7`，C22/S22=`2e−6/−7e−7`。调用 `NewtonianAttraction` 与 `HolmesFeatherstoneAttractionModel`。
- 相对论调用 `Relativity`、`LenseThirringRelativity`、`DeSitterRelativity`；太阳位置／速度复用 `OrbitForgeRelativityReference.java` 的明确合成星历，月球位置 `(3e8,−2e8,1e8)` m、GM=`4.902800118e12` m³/s²。日月引力使用 `ThirdBodyAttraction`。
- 无阴影 SRP 使用 `IsotropicRadiationSingleCoefficient`，4.56e−6 Pa、149597870700 m。阻力使用 `NRLMSISE00` / `IsotropicDrag`，F10.7 与平均通量均为 150、日 Ap=15。D=`Cd×0.02`，K=`Cr×0.03`；Cd/Cr 为 `(2.2,1.7)`、零和相应负值，独立变量直接是 D/K。
- 六种组合分别为重力单独，以及重力加相对论、加日月、加 SRP、加阻力、全部开启。该参考不包含 JR、潮汐、视太阳或阴影；它们分别由已有独立协议和下述链式测试覆盖。

固定参考 `tests/reference/propagators/hpop_force.txt` 的 ASCII / LF SHA-256：

```text
3e1b6fe27cfd6b66e14849aed2e49fb4ac71a84902b784a83c4ef2721d7df310
```

使用 `tools/validation/orekit/dependencies.json` 的固定 JAR，编译生成器及 `OrbitForgeRelativityReference.java`，运行主类 `OrbitForgeHpopForceReference <输出文件>`。例如 classpath 使用 `out/validation/orekit/jars/*`，编译输出写入独立的 `out/validation/<run>/java/`，运行时把它加入 classpath（Windows 分隔符 `;`，Linux `:`）。重复生成须字节一致；不让普通 CI 联网生成参考。

## 容限与已观测差异

`HpopForce.CombinedAndIsolatedTermsMatchIndependentOrekitAutomaticDifferentiation` 的逐分量加速度限为 3e−12 m/s²。位置块基础绝对限为 3e−18 s⁻²；含阻力时增加 `2e−7×|Jr_参考−Jr_重力|_max`，区分双精度重力运算和密度梯度误差。速度块限为参考块最大分量的 3e−11，加 1e−24 s⁻¹。D/K 限分别为向量最大分量的 3e−11 / 3e−13，加 1e−24 Pa。没有以总重力加速度的尺度验收小的速度／参数块。

当前 MSVC 最大位置块绝对差 `3.812e−21` s⁻²，速度块最大归一化差 `4.937e−14`，D/K 向量分别为 `4.535e−14` / `7.778e−16`。这验证固定输入下的算法装配，不是实际轨道精度。

## 其他链路与回归

- `Spice.SrpStateChainMatchesIndependentApparentSunAndForceDifferences` 的 48 组真实 CSPICE 状态链进一步接入总力 API；核对完整 SRP 速度块、K 和加上单极重力后的空间块。其独立差分与 641 组阴影局部参考见[视太阳协议](apparent_sun_differential_protocol.md)。
- `HpopForce.SecularAndSolidOceanCorrectionsRemainInTheRotatedHessian` 使用十年长期率、完整瞬时固体潮、独立 4×2 海潮截断与旋转地球，比较 500 / 1000 m 两档八阶笛卡尔差分，逐分量限 3e−17 s⁻²。人为放大常分潮使遗漏修正可辨识；这是装配检验，潮汐系数本身的独立 Orekit 参考继续保留。
- 错误测试检查无效状态、缺 UTC、极点偏导拒绝、无效星历 GM 先于位置查询、取消与重试，以及内置重力解析不修改调用方环境。名义与偏导加速度逐位一致。
- 对重构前后各自安装的 SDK 使用相同冻结合成环境，24 组带符号参数配置分别传播 ±10 s，共 48 个末端状态，位置／速度的 17 位文本逐字节一致。冻结矩阵与输入用于重构回归，不作为真实轨道的独立精度结论。
- 完整 Core 回归保留全部积分器、VOP、时间正则化、阴影和组合轨迹；集成回归覆盖天气／闰秒／预报节点、项目重开和资源冻结。完整 EGM2008 / FES 外部原文测试仅在显式提供文件时运行，条件跳过不能记作通过。
- 安装后的 SDK 实际调用总力接口与 Runtime 视太阳适配器，Python 回归检查现有 HPOP/SRP/VOP/时间正则化入口。本局部总力入口不另设桌面开关；联合协方差接口另见[矩阵验证协议](hpop_covariance_protocol.md)，当前 v41 / OBJS32 的保存重开、桌面编辑与报告由[项目验证协议](hpop_covariance_project_protocol.md)覆盖。

日志、XML、安装测试和逐次基线输出写入新的 `out/validation/` 目录；固定参考与生成器保留在仓库。尚无 STK 同版本、同数据配置的实测验收。
