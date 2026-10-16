# HPOP 时间正则化内核验证

本协议验证内部 Sundman 方程和共用积分内核。公共 HPOP 仍按普通时间传播；真实 TAI 端点、天气/机动边界、自适应正则化控制、API、项目保存及界面尚未接入。本项不能代替 STK 同配置轨迹验收。方程、尺度和后续边界见[内部契约](../architecture/time_regularization.md)。

## 独立参考

`tools/validation/orekit/OrbitForgeSundmanReference.java` 直接调用固定 **Hipparchus 4.0.2** 的 `IterativeLegendreGaussIntegrator`、`DfpField(32)` 和 `DormandPrince853FieldIntegrator`，不调用 OrbitForge。依赖下载地址与 SHA-256 在 `tools/validation/orekit/dependencies.json`；运行前逐个核验 JAR。

`tests/reference/propagators/sundman.txt` 的 ASCII/LF SHA-256 为 `8817fd254063bc4a8c635358b580f31e616b15832e6ac378ad34de8e0993478a`。普通测试只读取固定文件，不联网或重新生成。文件包含：

- 36 行 `P e n Q0`：e = 0、0.7、0.98、0.9999；n = 0.1、0.5、1、1.5、2、2.5、3、5、10。独立十六点 Legendre–Gauss 在偏近点角上求积，绝对/相对容差均 1e−14；生产使用 Simpson，n>2 时改用真近点角，避免两端复用同一求积实现。
- 54 行 `T kind e n q_end r0[3] v0[3] r[3] v[3] t`：两种力 × e=0/0.7/0.98 × n=1/1.5/2 × −0.375/0.4375/1.125 圈。输出时刻由七维方程积分得到，单位为真实秒；正则化坐标 q 无量纲。

μ = 3.986004415e14 m³/s²，近地点 7000000 m，半长轴 a=7000000/(1−e) m；初态在轨道面内转 0.4 rad，再绕 x 轴倾斜 0.6 rad。使用固定惯性直角坐标与连续相对秒，刻意隔离真实历表、UTC 和地球旋转。kind=0 为点质量；kind=1 加上 J2（J2=√5×0.484165371736e−3，R=6378136.3 m）以及各轴 `1e−7 sin(t/1900+0.7i) − 1e−12 v_i` m/s²。后者要求力使用物理时间与速度，不能把 q 或 dr/dq 代入。它是受控数值问题，不代表实际环境模型。

参考将长度按 a、时间按 T0=√(a³/μ) 无量纲化，用 32 位十进制 Dfp 运算，DP853 绝对容差 1e−20、相对容差 1e−18、最小步长 1e−16。每条正则化轨迹以 Q0/256 与 Q0/512 上限重算，保存细步结果；再以独立的普通时间方程传播到该真实秒，时间步上限为 2π/2048。生成器仅在两类检查均通过后写文件：

| 参考质量检查 | 位置范数差 m | 速度范数差 m/s | 时钟差 s |
| --- | --- | --- | --- |
| q 上限减半的实际最大值 | 3.14136e−8 | 1.84041e−12 | 2.03455e−9 |
| q 上限减半的拒绝界限 | 1e−6 | 1e−9 | 5e−9 |
| 普通时间方程交叉比较的实际最大值 | 1.57213e−5 | 9.53076e−9 | 不适用：指定相同时刻 |
| 普通时间方程交叉比较的拒绝界限 | 5e−5 | 5e−8 | 不适用 |

两种参考检查界限均不超过生产绝对误差预算的 1%。生产从圆整后的笛卡尔初态重建 a0/e0，参考归一化采用生成初态的 a/e；高偏心率下的初始能量舍入差也计入生产比较，未从误差中扣除。

## 生产内核验收

`All/SundmanMethods.FixedRegularizedStepsMatchIndependentHighPrecisionAndConverge` 使用与普通 HPOP 共用的 RK4、RKF7(8)、RKV8(9)、BS 和 GJ 内核。GJ 分别执行 Full / Pseudo，共六种设置，每种 54 行、粗细两组，合计 **648 次完整终点比较**。每条轨迹按 q_end 均匀划分，细组精确加倍粗组步数；检查 GJ 每条轨迹实际完成超过 100 个多步推进，避免只验到起步器。

| 方法 | 每圈粗步数；细步数加倍 | 最大位置范数差 m | 最大速度范数差 m/s | 最大时钟差 s |
| --- | --- | --- | --- | --- |
| RK4 | 262144 | 2.77991e−5 | 5.06138e−10 | 2.22353e−8 |
| RKF 7(8) | 4096 | 2.38487e−5 | 5.09532e−10 | 2.24682e−8 |
| RKV 8(9) | 4096 | 2.93436e−5 | 6.96225e−10 | 4.07454e−8 |
| BS 固定十八阶 | 2048 | 2.66512e−5 | 5.20493e−10 | 2.35159e−8 |
| GJ Full | 点质量 4096；时变力 16384 | 5.12312e−5 | 7.28061e−10 | 2.28175e−7 |
| GJ Pseudo | 点质量 4096；时变力 16384 | 5.09645e−5 | 7.24934e−10 | 2.27010e−7 |

所有粗细结果均须满足 **5 mm、5e−6 m/s、5e−7 s**；同条轨迹的步数加倍变化分别小于 **1 mm、1e−6 m/s、2.5e−7 s**。六种设置的步数加倍最大变化为 3.75930e−5 m、7.24049e−10 m/s、1.30851e−7 s。这些界限验证方程变换与积分一致性，不代表未来 TAI 端点拟合已达到纳秒精度，也不代表实轨预报精度。

高偏心率 n=2 的远地点附近，每个 q 步可跨越较长物理时间；时变摄动力额外要求足够取样。固定步 GJ 的校正收敛不保证截断误差合格，粗步可能失稳；无限增加步数又会进入时钟舍入平台。上述步数由保持原门槛的步数加倍检查确定，并非建议所有用户场景采用该默认值。普通时间 GJ 保持原有 64ε 收敛标准；七维混合方程的内核试验使用位置、dr/dq 范数及真实时间尺度的 8ε。起步器使用 BS 至少十四阶、相对容差 1e−14，位置/dr/dq/时钟绝对容差分别 1e−9/1e−8/1e−11。

另有独立解析和边界检查：

- n=1 / 2 分别对照偏近点角 / 真近点角的解析位置、速度和开普勒时间，三种偏心率、三种有符号圈数、BS/GJ 共 36 个终点，使用相同绝对门槛。
- 36 个每圈归一化值：相对界限 2e−12；e=0.9999 使用 2e−10，包含由圆整笛卡尔初态重建偏心率的条件数放大。此极限仅验证归一化，不扩大轨迹验收范围。
- 坐标往返、传入力的真实时间与速度；Pseudo 保留物理摄动并更新全部变换因子；非有限输入、无周期轨道、径向轨道和非法指数；中点子步取消不得发布部分状态。
- 克隆相同 GJ 历史后比较正反向单步：两种校正的状态相同，Pseudo 恰好少一次收敛后的完整力调用。分别长期传播后的历史与迭代次数可不同，不能从总次数差推导逐步上限。

## 普通 HPOP 回归与身份

为降低高阶外推的舍入放大，固定十八阶 BS 和 GJ 十四至十八阶起步器改用 2/4/6/8/12/16/24/32/48 子步，外推局部状态增量。普通自适应 BS 的 2/4/…/18 序列及其控制器保留。对应算法来源升级至 `bulirsch-stoer-midpoint-richardson-fixed18-v2`、`gauss-jackson12-summed-bs14to18-startup-full-v2` 和 `gauss-jackson12-summed-bs14to18-startup-pseudo-v2`；项目仍为 v35 / OBJS26。

普通时间的固定步长、Hipparchus/Orekit 轨迹、天气、缓存、科学插值、任务和 API 测试保持原门槛；v2 的普通时间数值见 [HPOP 协议](hpop_protocol.md#共用内核与外推-v2)。本次 Windows/MSVC Release 本地验证合计 Core 117 通过（全套后，以最终 13 项内核/正则化复核替代对应结果），集成 177 通过 / 2 项完整资源条件跳过，独立 Python 3.13 安装 217 通过 / 5 项可选 NumPy/pandas 跳过，安装后 SDK 2/2。没有新增界面，未重跑本轮无关的 DPI 验收。逐次 XML、日志、参考收敛输出及汇总保存在 `out/validation/`，不提交运行产物。

## 复现

Java 21 示例；`$jars` 指向已按 `dependencies.json` 核验的目录。新参考生成较慢，普通 CI 只消费仓库内固定输入。

```powershell
$jars = 'out/validation/orekit/jars'
$classpath = (Get-ChildItem -LiteralPath $jars -Filter '*.jar').FullName -join [IO.Path]::PathSeparator
New-Item -ItemType Directory -Force -Path out/validation/sundman/classes
javac -cp $classpath -d out/validation/sundman/classes tools/validation/orekit/OrbitForgeSundmanReference.java
java -Xmx768m -cp ("out/validation/sundman/classes" + [IO.Path]::PathSeparator + $classpath) OrbitForgeSundmanReference out/validation/sundman/sundman.txt
Get-FileHash -Algorithm SHA256 -LiteralPath out/validation/sundman/sundman.txt
cmake --build --preset desktop-graphics
ctest --preset desktop-graphics -R 'core\.(Sundman|GaussJacksonKernel|All/SundmanMethods)'
ctest --preset desktop-graphics -R 'core\.(Hpop|Mission)'
```

平台浮点运算可能有末位差异；比较完整状态及容限，固定参考文件不得为使测试通过而改写。完整集成、独立安装 Python 与 SDK 的命令遵循[构建说明](../development/build.md)和[CI 规范](../development/ci.md)。
