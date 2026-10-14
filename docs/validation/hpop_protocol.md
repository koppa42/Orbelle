# HPOP 验证协议

模型、输入约定与 STK 对照范围见[传播器](../architecture/propagators.md)。数值容限不等于真实轨道预报误差；当前没有 STK 直接参考结果。

## 固定数值参考

`tests/reference/propagators/hpop.txt` 为独立 Orekit 13.1.2 / Hipparchus 4.0.2 输出。参考生成器是 `tools/validation/orekit/OrbitForgeHpopReference.java`，依赖文件名及 SHA-256 固定在同目录 `dependencies.json`。输入为官方 ICGEM EGM96 全量文件，生成器校验其完整 SHA-256；生产表仅提取其中 20 阶，见 `tools/data/import_egm96.py`。

参考以 Holmes–Featherstone 重力和 DP853 数值传播，μ = 3.986004415e14 m³/s²、R = 6378136.3 m，tide-free、无其他摄动。DP853 最大步长 30 s，相对容差 1e-14，位置/速度绝对容差 1e-6 m / 1e-9 m/s。重力系刻意固定与 GCRF 同轴，避免把两个系统的 EOP/地球旋转差异混入重力、积分回归。

60 行涵盖低轨倾斜、GEO、近极轨、高偏心率、双曲线，阶次 2/0、8/8、20/20，时刻 ±6000 s 与 ±86400 s；每行包含完整初值、预期状态和初始加速度。CTest 使用生产默认 RKF78 容差，检查每分量位置 ≤0.15 m、速度 ≤0.00015 m/s、加速度 ≤5e-13 m/s²。参考文件不会在普通测试中自动重算。

复现（Windows 示例，Java 21，已核验 jars 放入指定目录）：

```powershell
$classpath = (Get-ChildItem out/validation/orekit/jars/*.jar | ForEach-Object FullName) -join ';'
javac -cp $classpath -d out/validation/hpop tools/validation/orekit/OrbitForgeHpopReference.java
java -cp "out/validation/hpop;$classpath" OrbitForgeHpopReference out/research/hpop/EGM96.gfc
```

极点本身由点质量加 J2 闭式梯度测试，避免依赖参考实现的轴上球坐标奇点。另测圆轨道解析极限、RK4 减半步长的四阶收敛、靠近 Epoch 范围端点的时间、1 ns 查询、乱序/重复/并发、取消后重用、错误容差、缺资源、最低高度和工作量上限。

## 集成与交付

- `HpopProject.*`：真实 ERFA/IERS 帧转换、原生 GCRF 的 EOP 身份、报告/AER 复用、初始历元覆盖与取消。
- `test_hpop.py`：Python 易用入口、不可变配置、Keplerian 的模型 μ、输出网格无关性、编辑前验证、准备结果独立和 v14 保存/打开。
- `project-current.orbitforge`：独立二进制固定输入中的 HPOP 参数精确往返；非当前版本拒绝。
- 安装后的 SDK 消费端：通过公共头文件创建 HPOP、查询并保存/重开，不依赖源码路径。
- `QuantityTest.HpopSettingsValidateCancelAndRoundTripWithoutApplyingTheOrbit`：配置草稿、阶次约束、错误校验、取消和重开；可用 `ORBITFORGE_HPOP_UI_CAPTURE` 输出宽/窄窗口截图，配合 DPI 设置检查。

运行日志与截图写入新 `out/validation/<运行名称>/`。修改力模型、系数、误差控制或分段策略时重新执行数值与交付检查；不因默认容差很小而提高产品精度宣称。
