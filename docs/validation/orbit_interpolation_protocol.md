# 科学插值验证协议

实现与 STK 参数语义见[科学轨道星历插值](../architecture/orbit_interpolation.md)。本协议验证显式表的计算层及 C++ / Python Session；HPOP 自动节点输出、项目和报告路由、VOP 与 STK 同配置结果尚未验收。

## 独立参考

`tools/validation/orekit/OrbitForgeInterpolationReference.java` 只调用固定 Hipparchus 4.0.2 的 `PolynomialFunctionLagrangeForm` 和 `HermiteInterpolator`，后者用 `UnivariateDerivative1` 同时提取函数值与导数。生成器不调用 OrbitForge。所需 `hipparchus-core-4.0.2.jar` 的来源与 SHA-256 固定在 `tools/validation/orekit/dependencies.json`，不增加生产依赖。

```text
javac -cp <jars>/hipparchus-core-4.0.2.jar -d <classes> tools/validation/orekit/OrbitForgeInterpolationReference.java
java -cp <jars>/hipparchus-core-4.0.2.jar;<classes> OrbitForgeInterpolationReference <output.txt>
```

Linux 将 classpath 分隔符 `;` 换为 `:`。先核验 JAR SHA-256，再生成两次，核对逐字节相同。输出使用 ASCII/LF，固定输入为 `tests/reference/propagators/orbit_interpolation.txt`，SHA-256 `8d8122d657c6ba438690e617c9e3ee22bd46a60207b22c42a818c87b6d6162fa`。

118 组 / 826 查询覆盖 Lagrange 0–29 和所有 Hermite 奇数 3–29，等间隔及交替 17/23 秒节点、节点命中、端点内侧 1 ns 和非整秒。输入包含倾斜圆轨道以及刻意与位置导数不一致的独立速度数据，后者仅用于验证 Lagrange 的独立速度语义。参考保存完整节点和预期状态，Core 与安装后的 Python 直接读取同一输入；统一加上 `8e18` TAI 纳秒的绝对历元，以验证先作时间差。方法一致性容限逐分量为位置 `2e-5 m`、速度 `2e-7 m/s`；这些不是低阶插值相对于真实轨道的误差保证。

## 解析与行为验证

- 不等间隔三次多项式：Hermite 3/5/7 同时恢复位置及导数；大绝对历元、零点两侧 1 ns；两者容限 `2e-9`（SI）。
- 倾斜圆轨道参考分别验证多项式算法；另以圆轨道解析解测量三阶/七阶、120/60 秒采样的误差收敛。七阶 60 秒最大位置范数误差须小于 `1e-4 m`，比 120 秒至少缩小 64 倍；同为 120 秒，七阶至少比三阶小 1000 倍。
- 精确节点逐位返回；Lagrange 速度不使用位置差分；包含左右包围节点的最近连续窗口、不均匀节点的局部区间及距离相等时的选择；零阶保持与间断列表。
- 共享端点前后 1 ns、机动后状态优先、缺口和覆盖外拒绝；空表、重复/乱序、重叠、参考系/原点不一致、非有限值、未知枚举及阶数不足拒绝；浮点无法分辨的时间窗口拒绝。
- 输入与选项所有权、创建和查询取消、取消后复用、乱序和并发；Session / Scenario 及隔离安装后的 Python、C++ SDK。
- 用 HPOP 点质量模型在 ±600 秒采样，再用两种七阶插值查询非节点，与同一 HPOP 直接积分比较：每轴 `1e-3 m`、`1e-5 m/s`。此项检查集成接口；独立数值真值来自上面的 Hipparchus 和解析参考，不把同实现对照当成外部验收。

运行日志、JUnit/GoogleTest XML、误差测量和安装产物写入新的 `out/validation/` 目录。禁止通过放宽容限掩盖实现误差。高阶病态数据不保证全局误差，数据分段政策和窗口选择仍需实测 STK；本协议不声称已完整实现 HPOP 科学插值。

本次数值核对的最大逐分量差：位置 `8.722417987883091e-8 m`、速度 `1.3387762010097504e-9 m/s`。七阶 60 秒圆轨道采样的最大位置范数误差分别为 Lagrange `2.284138628502547e-6 m`、Hermite `1.6854510426540375e-8 m`；减半步长与升阶门限均通过。运行日志保留在 `out/validation/hpop-interpolation-20261017/`，这些指标仅对应本协议的固定样例。
