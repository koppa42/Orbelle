# HPOP 验证协议

模型、输入约定与 STK 对照范围见[传播器](../architecture/propagators.md)。数值容限不等于真实轨道预报误差；当前没有 STK 直接参考结果。

## 固定数值参考

`tests/reference/propagators/hpop.txt` 为独立 Orekit 13.1.2 / Hipparchus 4.0.2 输出。参考生成器是 `tools/validation/orekit/OrbitForgeHpopReference.java`，依赖文件名及 SHA-256 固定在同目录 `dependencies.json`。输入为官方 ICGEM EGM96 全量文件，生成器校验其完整 SHA-256；生产表仅提取其中 20 阶，见 `tools/data/import_egm96.py`。

参考以 Holmes–Featherstone 重力和 DP853 数值传播，μ = 3.986004415e14 m³/s²、R = 6378136.3 m，tide-free、无其他摄动。DP853 最大步长 30 s，相对容差 1e-14，位置/速度绝对容差 1e-6 m / 1e-9 m/s。重力系刻意固定与 GCRF 同轴，避免把两个系统的 EOP/地球旋转差异混入重力、积分回归。

60 行涵盖低轨倾斜、GEO、近极轨、高偏心率、双曲线，阶次 2/0、8/8、20/20，时刻 ±6000 s 与 ±86400 s；每行包含完整初值、预期状态和初始加速度。CTest 分别使用生产默认 RKF78 / RKV89 / Bulirsch–Stoer 容差及 Gauss–Jackson 的 10 s 固定步长，检查每分量位置 ≤0.15 m、速度 ≤0.00015 m/s、加速度 ≤5e-13 m/s²。参考文件不会在普通测试中自动重算。

复现（Windows 示例，Java 21，已核验 jars 放入指定目录）：

```powershell
$classpath = (Get-ChildItem out/validation/orekit/jars/*.jar | ForEach-Object FullName) -join ';'
javac -cp $classpath -d out/validation/hpop tools/validation/orekit/OrbitForgeHpopReference.java
java -cp "out/validation/hpop;$classpath" OrbitForgeHpopReference out/research/hpop/EGM96.gfc
```

极点本身由点质量加 J2 闭式梯度测试，避免依赖参考实现的轴上球坐标奇点。另测圆轨道解析极限、RK4 减半步长的四阶收敛、靠近 Epoch 范围端点的时间、1 ns 查询、乱序/重复/并发、取消后重用、错误容差、缺资源、最低高度和工作量上限。

## RKF 7(8)

按 [STK 13.1 Integrator](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm) 的定义推进七阶解，以八阶嵌入解估计误差。系数核对 [Fehlberg NASA TR R-287](https://ntrs.nasa.gov/api/citations/19680027281/downloads/19680027281.pdf) 的 Table X（印刷第 65 页，PDF 第 72 页）；原 PDF SHA-256 为 `5553a2a3eb53785a461762cc2b29428015f1b32c3ad0a5cb57f85a421256a0c8`。零基阶段 0、10 的七阶权重为 41/840，阶段 11、12 为 0；误差用 `abs(h × 41/840 × (k11 + k12 − k0 − k10))`，控制指数为 1/8。运行时来源为 `rkf78-7th-fehlberg-r287-v1`，明确区别先前的八阶推进；不改变公开 `rkf78` 枚举或项目 v33 / OBJS24 的布局。

成熟算法对照复用下述 Hipparchus 4.0.2 GBS 的 60 组固定输出和独立生成器，SHA-256 为 `62e89df62c29838d5fd03e37ce73ffac28632be40467cafeb4bbccb93ce6885b`。包含圆轨道、偏心率 0.7/0.98、J2、随时间变化的月球差分引力、正反向一天、非整数秒和 1 ns；GBS 参考已通过 60/30 s 收敛和 SciPy DOP853 交叉核验。生产 RKF 使用最大 300 s、相对容差 1e−14、位置/速度绝对容差 1e−8 m / 1e−11 m/s，保持 5 mm / 5e−6 m/s 三维验收线。Windows/MSVC 最大位置差 2.475 mm、速度差 2.619e−6 m/s。既有 Orekit Holmes–Featherstone/DP853 的 60 组重力轨迹使用默认容差也通过原验收线。

独立 Kepler 解析解采用偏心率 0.7、近地点 7000 km、600 s 弧段。以松容差和相同的最小/最大步长确保 150/75 s 两组等步积分，误差比须在 115–150 内，七阶预期为 128；实测约 133.411，细步位置误差约 7 µm。先用旧八阶生产权重运行同一测试，得到 243.105 并失败，确认该测试能区分推进成员。另验证拒步、无法达到精度的最小步报错、取消后的完整缓存复用、乱序/并发、大 Epoch 和纳秒查询。该阶次测试不是公开的固定步长模式，后者仍待实现。

Qt 沿用自适应积分器交互验收，增加 RKF 7(8) 的键盘顺序、摘要、非法步长、取消/重开及 430/720 逻辑宽度；Windows 实际 DPR 1.5/2.25 的四张 RKF 截图均已检查。误差范数、步长控制器和 STK 同配置轨迹尚未直接对照，七阶推进对齐不能代替完整 STK 验收。

## RKV 8(9) Efficient

系数来自 rklib 的 `rkv98e`，固定提交 `a1bf2d26ba2c37d049b448d28d4e9800a0ee4b58`；源文件 `src/rklib_variable_steps.f90` 的 SHA-256 为 `815a58549a72675c871f88abe127ad4444bcecfa20d9ef0a7b11ea58f8deb33e`。保留其 BSD-3-Clause 许可并随 Python、SDK、Windows 包交付。按照 STK 的八阶推进描述，生产使用原表的 `d` 权重，而原 rklib 常规九阶推进使用 `c` 权重；误差为 `h Σ(c−d)k`。STK 的内部控制器和同配置轨迹尚无直接对照结果。

`tools/validation/rkv89_reference.py` 使用 NumPy 2.3.3 / SciPy 1.16.3 DOP853，不导入 OrbitForge 或其系数表。`tests/reference/propagators/rkv89.txt` 的 SHA-256 为 `da35cab3683d41c18b02f46585c42812836bc66637b8797c6554c1cb8ae4f985`，60 行记录完整初值和终态：圆轨道、偏心率 0.7 / 0.98 椭圆、LEO / MEO 闭式 J2、J2 加解析运动月球的差分引力。时间覆盖 ±1 天、±6 小时、±2 小时、非整数秒、0.25 秒及 1 ns；所有位置为固定惯性轴的 SI 分量。EOP 取恒等旋转，太阳/月球摄动之外的力关闭，避免混入外部数据差异。月球轨迹使用 384400000 m 半径、27.321661 日周期、0.3 rad 倾角、0.4 rad 初相位，GM 为 4.902800066e12 m³/s²；这里用于检验非自治方程阶段时刻，不是真实月球星历。

参考 DOP853 相对容差 3e−14，位置/速度绝对容差 1e−9 m / 1e−12 m/s；最大步长 10 s，另以 20 s 复算。两次参考最大位置差 1.40e−5 m、速度差 1.51e−8 m/s，分别须小于 5e−5 m / 5e−8 m/s。生产 RKV89 设置最大 300 s、相对 1e−14、位置/速度绝对容差 1e−8 m / 1e−11 m/s，正反向三维位置误差限 5 mm、速度限 5e−6 m/s。Windows/MSVC 实测最大 0.507 mm / 5.43e−7 m/s。数值容差是每步约束；例如将同一天圆轨道的绝对容差放宽到 1e−5 m / 1e−8 m/s、相对 1e−13，可产生约 7.5 cm 的累计误差，不能据容差值宣称全局毫米精度。

八阶成员选择另由偏心率 0.7 的 Kepler 解析解验证：近地点 7000 km，传播 6000 s，松容差固定 300 s 和 150 s 两组积分步，位置误差比须在 220–330（八阶预期 256）内，排除误用九阶推进权重；150 s 误差小于 1 mm。另验证拒步缩步、最小步长无法满足精度时报错、初始历元与 1 ns、Epoch 大值、取消后的完整段复用、乱序/并发查询及阶段星历范围。既有 Orekit Holmes–Featherstone / DP853 的 60 行重力参考也对 RKV89 执行相同校验。Python 磁暴边界测试补充 RKV89 的 60/7 s 步长对照，仍以 RK4 0.5 s 作为收敛参考。

```powershell
python tools/validation/rkv89_reference.py out/validation/rkv89-reference
ctest --preset desktop-graphics -R "core.Hpop."
```

项目 v33 / OBJS24 保存 RKV 枚举 2；公共接口测试单独往返 RKV 设置，当前 Gauss–Jackson 固定样例由 C++ / Python 逐字节往返；未知枚举和旧格式拒绝且不改源文件。Python / SDK 验证公共枚举、高层设置、分析身份、旧快照不变和重开重现；Qt 验证 RKV 自适应字段、非法步长提示、键盘选择、取消与重开、轨道摘要以及 430 / 720 逻辑像素和 DPR 1 / 1.5。安装检查还核验 rklib 许可原始字节。

## Bulirsch–Stoer

`tools/validation/orekit/OrbitForgeBsReference.java` 直接调用固定 Hipparchus 4.0.2 `GraggBulirschStoerIntegrator`，JAR 散列沿用 `dependencies.json`。生产实现只依赖标准库，不调用该参考库。参考输入复用上述 SciPy 基准文件中的 kind、时间、初始六维状态；生成器忽略其中的终态，独立实现点质量/J2/运动月球闭式右端项。参考 GBS 保持默认阶数与控制器，关闭插值误差控制并直接积分至每个输出时刻，使用 1e−14 相对容差和 1e−9 m / 1e−12 m/s 绝对容差。最大步长 60/30 s 复算的最大位置/速度差为 3.516e−5 m / 3.679e−8 m/s，均小于 5e−5 m / 5e−8 m/s；与独立 SciPy DOP853 10 s 结果交叉比较为 3.497e−5 m / 3.674e−8 m/s。更小的 20/10 s GBS 步长在一天积分中进入舍入误差主导区，差值约 7.92e−5 m；不能假定不断减小步长会持续提高精度。

固定输出 `tests/reference/propagators/bulirsch_stoer.txt` 包含 60 组正反向、非整数秒和 1 ns 参考，SHA-256 `62e89df62c29838d5fd03e37ce73ffac28632be40467cafeb4bbccb93ce6885b`。生产使用最大步长 300 s、相对 1e−14、位置/速度绝对容差 1e−8 m / 1e−11 m/s；三维误差限 5 mm / 5e−6 m/s。Windows/MSVC 实测最大位置差 0.117 mm、速度差 1.230e−7 m/s。既有 60 组 Orekit Holmes–Featherstone / DP853 重力参考同时加入 Bulirsch–Stoer，包含双曲线与 20×20 引力场。

偏心率 0.7、近地点 7000 km、6000 s Kepler 解析解在最小/最大步长均为 300 s 时检验自适应阶数和精度：收紧容差后误差须至少下降 100 倍且小于 1 mm，记录中点子步工作量，不能固定在低阶后仅靠标签声称自适应。另测 300/60 s 最大步长收敛；快速双曲离去在固定 300 s 下精度不足必须报错，允许缩步后应成功。取消覆盖一次外推尝试内部；取消后重用、乱序、并发和大 Epoch 查询保持确定性。天气磁暴边界用 60/7 s Bulirsch–Stoer 与 RK4 0.5 s 比较，正反向位置差限 0.1 mm。

```powershell
javac -cp $classpath -d out/validation/bs tools/validation/orekit/OrbitForgeBsReference.java
java -cp "out/validation/bs;$classpath" OrbitForgeBsReference tests/reference/propagators/rkv89.txt out/validation/bs/bulirsch_stoer.txt
ctest --preset desktop-graphics -R "core.Hpop."
```

项目 v33 / OBJS24 保存 Bulirsch–Stoer 枚举 3，公共接口测试单独往返其配置；当前固定样例使用 Gauss–Jackson，并独立核对两层 CRC；未知枚举与旧版本拒绝。Python 与安装 SDK 核验保存、计算身份及旧分析不变；Qt 同时验证两种新增自适应方法、键盘顺序、非法步长、取消/重开、摘要和窄窗/DPI。STK 同配置轨迹、VOP、固定步长外推及科学插值未计入本项完成范围。

## 文件与高阶重力

`tests/reference/propagators/gravity.txt` 是独立 Orekit 13.1.2 Holmes–Featherstone 输出，生成器为 `tools/validation/orekit/OrbitForgeGravityReference.java`，复用上述固定 jars。使用明确构造的归一化系数，覆盖 21、70、180、360 阶、全阶与带谐，以及一般位置、低轨、GEO、南北近极点，共 40 个加速度参考；每分量绝对容限 5e-12 m/s²。这个容限用于双精度不同递推顺序的数值回归，不代表模型物理精度，也不代替 EGM2008 轨道对照。

```powershell
javac -cp $classpath -d out/validation/hpop tools/validation/orekit/OrbitForgeGravityReference.java
java -cp "out/validation/hpop;$classpath" OrbitForgeGravityReference
```

`GravityFiles.*` 验证 `.grv` 归一化／非归一化、ICGEM 默认归一化、D 指数、原始换行哈希、截断／重复／NaN／不支持块、取消保留、不可变资源及项目更新确认。低层报告和场景也拒绝与项目记录不匹配的重力 Provider。`test_gravity.py` 验证目录安装、21 阶与完整 2190×2159 Python 配置、准备、保存和重新打开，并拒绝超出文件实际次数的配置。

官方文件另做完整下载与原文校验；不把数百 MiB 文件提交仓库或作为普通 CI 的联网依赖。可选 `DataCenterTest.LiveOfficialGravityDownload` 通过 `ORBITFORGE_LIVE_GRAVITY=egm2008` 或 `egm96` 执行真实 Qt 下载、全量安装及再次检查；未设置时跳过。桌面离线测试覆盖后台导入和失败保留、重力分组、静态有效期、2190 阶／2159 次独立限幅、键盘输入、窄窗草稿保留、取消重开与缺资源错误。截图入口沿用 `ORBITFORGE_DATA_UI_CAPTURE` 和 `ORBITFORGE_HPOP_UI_CAPTURE`。

### Gauss–Jackson 12

[STK 定义](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)要求十二阶、固定步长，以及收敛后 Full/Pseudo 两种加速度更新。生产采用 [Berry–Healy 求和形式](https://hdl.handle.net/1903/2202)，启动和尾段使用已验证的 Bulirsch–Stoer，保持至少十四阶。`tools/validation/gauss_jackson_coefficients.py --check` 独立从有理数递推生成四张 13 项系数表，验证 52 个精确多项式条件及论文中两项八阶系数；普通运行不依赖 Python 或第三方积分库。

复用前节 SHA-256 固定的 60 组 **Hipparchus 4.0.2 GBS** 输出，覆盖正反向、一天传播、e=0.7 / 0.98、J2、时变月球引力、非整数秒及 1 ns。Full / Pseudo 各以固定 30 s 传播，再减为 15 s；每种设置的三维位置/速度误差限保持 **5 mm / 5e−6 m/s**，30/15 s 位置变化限 **1 mm**。参考本身已通过 GBS 60/30 s 与独立 SciPy DOP853 的收敛对照，所有初值、力模型、时间和参考系与前节一致。 Windows/MSVC 实测两种模式最大位置差均约 0.498 mm、最大速度差 3.869e−8 m/s；15 s 参考位置差约 66 µm，30/15 s 最大变化约 0.467 mm。

60 s 初试在最高偏心率的一天轨迹上出现约 1.14 m 差异，不满足验收；记录该结果并减小固定步长，未放宽误差限。该方法不自动控制截断误差，不能把某个轨道上的固定步长推广到其他轨道。圆轨道 60000 s 的阶数检查使用 200 / 160 s，以避开 240 / 120 s 细步接近舍入误差的情况，记录 `log(error_coarse/error_fine) / log(200/160)`，接受 11–14 的渐近阶数范围，并要求细步误差小于 1 mm；Windows/MSVC 观察阶数为 12.78，细步误差约 0.150 mm。

Core 另检查固定网格缓存跨段、7 s / 10.3 s 等不整除 300 s 的步长、初值和 Epoch 大值、取消启动及已建立历史后的重用、并发和乱序查询。J2 场统计 Full / Pseudo 的完整力调用次数并限制位置差，不能用省略全部摄动来制造性能提升。Mission 复用解析霍曼转移，检查脉冲后的新 HPOP 段不继承旧历史。原有 60 组 Orekit 球谐轨迹也包含 Gauss–Jackson。

Python 把 NRLMSISE-00 15→400 Ap 跳变前后的完整链与 RK4 0.5 s 对比，Gauss–Jackson 使用 1 s Full、0.7 s Pseudo，跳变前已有完整多步历史；正反向位置变化限保持 0.1 mm。公共接口检查高层字符串与原生枚举、冻结分析、保存重开、活动固定步长/校正方式的身份变化，以及无效自适应草稿不改变结果和报告缓存。

项目 v33 / OBJS24 的独立 1458-byte 样例使用 Gauss–Jackson + Pseudo，C++/Python 逐字节往返并拒绝未知校正枚举、旧版本及 CRC 完整的损坏输入。SDK 消费端检查相同模式及算法身份。Qt 检查仅该方法显示的校正选项、固定步长错误、键盘选择、草稿保留、取消和重开；Windows 原生窗口覆盖 430 / 720 逻辑像素，实际 DPR 以日志为准。

```powershell
python tools/validation/gauss_jackson_coefficients.py --check
ctest --preset desktop-graphics -R "core.(Hpop|Mission).|QuantityTest.Hpop"
```

STK 原始轨迹、阴影专用事件、VOP、时间正则化、科学插值以及 86400 s 上限仍不属于此项已验收范围。运行命令、指标 XML、失败与修正日志、原生窗口截图进入 `out/validation/`。

## 完整 EGM2008 与 2190 阶验证

`OrbitForgeFullGravityReference.java` 直接调用未经修改的 Orekit 13.1.2 `HolmesFeatherstoneAttractionModel`、`ICGEMFormatReader` 和 Hipparchus 4.0.2 DP853。生成器不导入 OrbitForge，也不转录生产递推。依赖散列使用同目录 `dependencies.json`；参考字节保持 UTF-8/LF。

- `gravity_full.txt`：120 个加速度与 2 条 ±120 s 轨迹。包含稠密 2190×2190、2190×2159，以及刻意放大最高阶带谐、近带谐和扇谐的压力场；截断覆盖 720、1440、2190 阶，0 次和完整次数。
- `gravity_egm2008_full.txt`：从完整官方 EGM2008 原文读取 80 个加速度与 2 条 ±120 s 轨迹；截断覆盖 20、360、720、1440、2190 阶，0 次和完整次数。输入 252140870 bytes，SHA-256 为 `ab5b524da073e63b5bdceb7ca47a0de07a26dd44a1c5798f39fc98dc80af70fd`；生成器及集成测试都核对散列。有效模型范围 2190×2159 与 [NGA 模型说明](https://earth-info.nga.mil/?action=wgs84&dir=wgs84)一致。该 ICGEM 导出把 2160–2190 次显式补零，因此文件记录范围是 2190×2190；加载器完整校验并保留该范围，另断言全文件求值与 2190×2159 求值相同，不把填零当成额外模型精度。
- 八个加速度位置覆盖赤道近地表、一般位置、GEO、南北近极点，以及生产递推切换点两侧；单位为地固 SI，地球固定与 GCRF 同轴。完整文件测试不增加联网依赖：设置 `ORBITFORGE_FULL_GRAVITY_FILE` 指向已校验的本地文件后运行 `GravityFiles.FullOfficialEgm2008MatchesIndependentOrekit`，未提供时明确跳过。稠密与放大系数测试始终离线运行。
- 全部加速度每分量容限 5e-12 m/s²；全阶轨迹每分量位置 1e-4 m、速度 1e-7 m/s。初始 TAI 历元 Epoch(0)，位置 `(6408136.3,0,0)` m、速度 `(0,6200,4700)` m/s，只包含选定静态重力。DP853 最大步长 10 s、相对 1e-14、位置/速度绝对容差 1e-7 m / 1e-10 m/s；生产 RKF78 最大步长 5 s、相对 1e-14、位置/速度绝对容差 1e-8 m / 1e-11 m/s。收紧积分设置用于隔离全阶力模型误差，不改变生产默认设置，也不把局部容差解释为全局精度。
- 极点本身另用 `P_n(1)=1`、`P'_n(1)=n(n+1)/2` 的闭式值核对，70 位 Decimal 生成器为 `tools/validation/gravity_pole.py`。它区分最高阶真实横向梯度与数值奇点，包含正负零输入一致性检查。

复现命令（`$classpath` 为已核验 JAR 的平台分隔列表）：

```powershell
javac -cp $classpath -d out/validation/full-gravity tools/validation/orekit/OrbitForgeFullGravityReference.java
java -Xmx1536m -cp "out/validation/full-gravity;$classpath" org.orekit.forces.gravity.potential.OrbitForgeFullGravityReference
java -Xmx1536m -cp "out/validation/full-gravity;$classpath" org.orekit.forces.gravity.potential.OrbitForgeFullGravityReference out/research/hpop/EGM2008.gfc
python tools/validation/gravity_pole.py
$env:ORBITFORGE_FULL_GRAVITY_FILE = (Resolve-Path out/research/hpop/EGM2008.gfc)
ctest --test-dir out/build/desktop-graphics -C Release --output-on-failure -R 'Gravity[.]|GravityFiles[.]'
```

这些数值只验证匹配输入下的模型与积分一致性，不是 STK 同配置实测，也不代表真实轨道预报误差。完整文件不提交仓库，固定结果与复现程序保留在版本库。

## 日月引力与星历

`tests/reference/propagators/third_body.txt` 是独立 Orekit 13.1.2 `ThirdBodyAttraction` / Hipparchus 4.0.2 DP853 的 32 组输出。生成器 `tools/validation/orekit/OrbitForgeThirdBodyReference.java` 使用显式定义的移动太阳/月球、点质量地球，隔离差分引力和积分的实现；不依赖 OrbitForge 力值或 STK 输出。

覆盖太阳单独、月球单独、组合、两者 GM 覆盖四种情况，LEO / GEO 初值及 ±6000 s / ±86400 s。参考每分量容限：初始第三体加速度 3e-18 m/s²，传播位置 2 mm、速度 2 µm/s。生产 RKF 7(8) 使用相对容差 1e−13、位置/速度绝对容差 1e−5 m / 1e−8 m/s，最大步长分别为 30/15 s；两组都检查原有独立参考容限，另要求每分量位置的步长收敛差小于 0.1 mm。Windows/MSVC 实测最大位置误差 1.968e−5 m、速度误差 1.191e−8 m/s，30/15 s 最大位置差 2.362e−5 m。此前七阶推进在 60 s 下的一天弧位置误差约 2.27 mm，超出 2 mm 标准，因此用步长收敛降低积分误差，保持参考值及验收阈值。具体数值固定在 `third_body_test.cpp`；不把该回归精度宣称为真实预报精度。另测毫米位移的潮汐极限、地心零差分、奇点、非法 GM、禁用无查询、取消后重用及覆盖先于力评估失败。

```powershell
javac -cp $classpath -d out/validation/hpop tools/validation/orekit/OrbitForgeThirdBodyReference.java
java -cp "out/validation/hpop;$classpath" OrbitForgeThirdBodyReference
```

真实 SPICE 路由另用仓库内 DE440s 衍生片段检查位置、数据身份、保存/重开与原分析不受文件更新影响。构造两个不相连的 SPK 时间段，验证端点各自可查询也不能通过整段覆盖检查。统一覆盖约束含初始状态至全部输出的传播路径；不接受星历外推。

可选 `DataCenterTest.LiveOfficialEphemerisDownloadAndRecheckRetainCurrentData` 由 `ORBITFORGE_LIVE_DE440S=1` 启用：完整官方文件下载、哈希和目标链校验、再次条件请求、错误替换保留已安装文件。不将 32 MiB 原文加入仓库，也不让普通 CI 依赖联网。运行 Qt 网络验证需加载对应 TLS 插件。

桌面用 `QuantityTest.HpopThirdBodiesPreserveDraftsAndValidateManualConstants` 检查 GM 配套/手动切换、只读常数的单位转换、数据中心返回后保留草稿、非法值及取消；`DataCenterDialog.EphemerisGroupAndDetailsRemainReadableAtCompactWidth` 检查分组、窄窗和下载入口。沿用上述截图环境变量，并在真实主题、可显示中文的字体及不同 DPI 下检查。

## 太阳辐射压

模型边界及常数见[太阳辐射压](../architecture/solar_radiation_pressure.md)。`srp.txt` 使用独立 Orekit 13.1.2 `SolarRadiationPressure`／Hipparchus 4.0.2 DP853，显式移动几何太阳、点质量地球及球面食模型，覆盖 LEO/GEO、±6000 s／±86400 s、Cr／面积质量比及 23 km 大气高度，共 24 个轨迹参考。太阳初相位为 0.03 rad，绕行周期 365.25 d、倾角 0.409 rad；完整参数固定在生成器中。

参考 DP853 最大步长 1 s、位置／速度绝对容差 1e-8 m／1e-11 m/s、相对容差 1e-14，食事件阈值 1e-8 s。生产 RKF78 对照采用最大步长 2.5 s 和相同误差容差；每分量位置容限 0.5 mm、速度 0.2 µm/s、初始光压加速度容限 1e-20 m/s²。参考从 10 s 收紧到 1 s 的独立收敛检查排除参考积分误差；不使用放宽容限代替收敛检查。

```powershell
javac -cp $classpath -d out/validation/hpop tools/validation/orekit/OrbitForgeSrpReference.java
java -cp "out/validation/hpop;$classpath" OrbitForgeSrpReference
python tools/validation/generate_srp_geometry_reference.py
```

`srp_geometry.txt` 有 182 个独立均匀视圆盘面积参考，含太阳／地球／月球三圆交与并集、重合、包含、内外切、环食及大小相差悬殊的随机配置。独立 Python 生成器沿垂直截面积分，与生产 Green 定理圆弧积分采用不同方法；照明比例绝对容限 2e-9。Core 另检查距离平方、带符号 Cr、零面积、圆柱、大气高度、奇点、缺月球、无阴影不查询月球，以及光压单独启用时的完整星历覆盖。

`SrpPersistence.*`／`SrpFiles.*` 和 `test_srp.py` 检查共享会话／分析／报告、真实 DE440s 路由、v33 保存与重开、有效参数身份及原分析不可变。桌面 `QuantityTest.HpopSrp*` 检查单位转换、有效遮挡体组合、窄窗、草稿、错误路由及取消；截图使用 `ORBITFORGE_HPOP_UI_CAPTURE`，Windows 原生平台分别设置 `QT_SCALE_FACTOR=1`／`1.5`，日志记录实际窗口 DPR。

## 集成与交付

- `HpopProject.*`：真实 ERFA/IERS 帧转换、原生 GCRF 的 EOP 身份、报告/AER 复用、初始历元覆盖与取消。
- `test_hpop.py`：Python 易用入口、不可变配置、Keplerian 的模型 μ、输出网格无关性、编辑前验证、准备结果独立和 v33 保存/打开。
- `project-current.orbitforge`：独立二进制固定输入中的 HPOP 参数精确往返；非当前版本拒绝。
- 安装后的 SDK 消费端：通过公共头文件创建 HPOP、查询并保存/重开，不依赖源码路径。
- `QuantityTest.HpopSettingsValidateCancelAndRoundTripWithoutApplyingTheOrbit`：配置草稿、阶次约束、错误校验、取消和重开；可用 `ORBITFORGE_HPOP_UI_CAPTURE` 输出宽/窄窗口截图，配合 DPI 设置检查。

运行日志与截图写入新 `out/validation/<运行名称>/`。修改力模型、系数、误差控制或分段策略时重新执行数值与交付检查；不因默认容差很小而提高产品精度宣称。

## 逐天体与 GM 来源验证

Core 的 `EveryBodyAndSourceActuallyChangesTheIntegratedForce` 将十个天体分别通过三种 GM 来源积分，并用独立的扩展精度差分公式核对速度增量（2e-14 m/s² 的分量容限），验证地心加速度项和逐阶段 Provider 路由。此短步测试验证力模型装配；长弧日月仍用已有 32 组 Orekit 参考验证。

`data/samples/spice/de440s_planets_2016_leap` 从固定原始 DE440s 保留十四条链，包括 199→1→0、299→2→0、399→3→0、301→3→0、火星与外行星系统质心→0。集成与 Python 测试检查全部天体传播、原始日月样例缺少行星时的失败、项目重开和 Cb 常数快照。CSPICE 合成断档测试覆盖 Mercury、Jupiter 的内部空隙与缺失 Saturn，不能仅查询区间端点。

使用 `compare_spice_ephemeris.py --all-planets` 对全部十四组状态与原始 DE440s 的独立 jplephem 读取比较；每分量位置容限 1 mm，速度 1e-7 m/s。固定输入、覆盖及单位说明见样例 README。每次结果写入新的 `out/validation/`；这些测试不代替相同资源及设置的 STK 轨迹比较。

大气阻力通过 `HpopOptions.drag` 接入，低层 `evaluate_atmosphere` / `evaluate_drag` 使用 SI 输入；实际 HPOP 同时保留 UTC/EOP/Sun。模型、跨接口和独立参考见[阻力契约](../architecture/atmospheric_drag.md)及[阻力验证](../validation/atmospheric_drag_protocol.md)。

### 光压太阳位置

- `Spice.MovingSunLightTimeAndAberration` 独立生成 Earth/SSB 与 Sun/SSB 线性 SPK，解析计算一次光行时及速度横向分量的光行差旋转。顺行、逆行、零相对速度使用 1 mm 向量容限（约 1e-14 相对舍入界）；人工放大的 LSK 时间变化验证 TAI/TDB 导数的方向与实际应用，不作为物理星历参考。
- `Spice.MovingSunCertifiesHistoryAndGaps` 检查接收端点有数据而过去 Sun 链有洞、过去 Earth 数据缺失但不需要、接收区间内部缺口、反序和策略越界。远距离观测者所需发光时刻越界必须失败；失败后合法查询仍可用。
- `SolarRadiationPressure.SunMethodsUseStageObserversAndKeepGravityGeometric` 检查三种方法真实进入正反向传播、卫星状态按积分阶段更新、第三体几何引力查询独立；不支持视太阳的 Provider 必须拒绝对应方法。
- Python `test_srp.py` 使用固定 DE440s：三个方法分别改变轨迹/身份，保存重开精确复现，旧分析保持不变；运动与静止观测者不能返回相同视方向。原有 Orekit 几何基准显式选择 True，不用改基准值掩盖默认方法变化。
- v33 / OBJS24 独立固定样例保存禁用的 Apparent 草稿；重算 CRC 后的未知太阳方法依然拒绝。安装后 SDK 调用新接口；桌面验证三选项、默认值、键盘切换、宽窄/DPI、保存后重开和取消。

上述为公开 NAIF 模型及路由验证；STK HPOP 同版本、同配置的轨迹验收仍未完成，遮挡体视位置和 Boundary Mitigation 单独列为待实现。

## 固体潮

计算边界、IERS 常数与容限见[固体潮契约](../architecture/solid_tides.md)。新增 `SolidTides.*` 核验 120 个独立加速度和四条正反向轨迹；`test_solid_tides.py` 检查真实 DE440s/EOP、零直接第三体、三种潮汐模式、报告、来源身份、非活动草稿和保存/重开；当前格式样例包含 Full + 截断 + 附加时间项的 0/0 草稿。SDK 消费端直接调用加性系数接口。`QuantityTest.HpopSolidTides*` 覆盖三选项、截断联动、键盘、宽窄/DPI、重开/取消及缺少星历时返回引力页。

独立数据生成使用 `tools/validation/orekit/dependencies.json` 中八个已验 SHA-256 的 JAR。将它们加入 Java classpath，运行 `javac -cp <classpath> -d <output> tools/validation/orekit/OrbitForgeSolidTideReference.java`，然后运行 `java -cp <classpath+output> org.orekit.forces.gravity.OrbitForgeSolidTideReference`。classpath 分隔符按平台使用 `;` 或 `:`。生成器置于 Orekit 包名仅为调用其包级 `SolidTidesField`，没有修改或复制该库算法；Love 数来自该固定版本的 IERS_2003 资源，频率/极潮函数明确关闭。

固定文本参考按 `.gitattributes` 保存为 UTF-8 / LF；Windows Java 输出先只规范换行再记录散列，数值文本保持原样。

自定义 Love 数的独立参考为 `solid_tides_love.txt`：48 组系数、96 组加速度及 16 条 ±1 天轨迹，覆盖四个 k20、两种底场潮汐系统、截断、近极点、频率/极潮开关，沿用现有容限。生成器 `OrbitForgeLoveNumberReference.java` 与上述两个生成器一起编译，运行 `org.orekit.forces.gravity.OrbitForgeLoveNumberReference`。`GravityFiles.*` / Python 检查 `.grv` 两种 Love 数语法、冲突与损坏、导入失败/取消、收据重开、数据确认及分析不可变；Qt 检查选文件时显示值变化和取消，SDK 直接调用自定义 k20 的永久潮 API。

附加时间项另有 39 组独立系数、24 组加速度及四条轨迹对照（`solid_tides_time_dependent.txt`），包括 IERS 2003 与 2010 极潮符号差异、历史平均极点边界、UT1/TT 时间约定和非活动开关身份。Python 核验 Full 开关切换、数据覆盖、报告与存档；Qt 核验附加时间项仅 Full 可操作、切换模式保留草稿、宽窄/DPI、确认与取消。全部来源和容限见固体潮契约。

## 相对论

IERS 2003 三项修正的公式、时间/坐标约定、独立 Orekit 生成器及分项/轨迹容限见[相对论契约](../architecture/relativity.md)。`Relativity.*` 比较 45 组单项与总加速度、8 条正反向轨迹；实际 DE440s、解析 TDB/TAI 速度转换和 EOP 路由独立验证。安装后的 Python/SDK、当前格式固定样例、Qt 实际窗口及错误/取消路径均属于本项验收。尚无同配置 STK 导出真值。

## STK 球形模型的卫星质量

`tools/validation/orekit/OrbitForgeMassReference.java` 使用固定 Orekit 13.1.2 / Hipparchus 4.0.2；与 `OrbitForgeDragReference.java` 一起编译。JAR 校验值见同目录 `dependencies.json`。运行 `OrbitForgeMassReference` 生成 `tests/reference/propagators/mass.txt`，禁止从生产代码生成真值。

参考向 Orekit 的 `IsotropicDrag` / `IsotropicRadiationSingleCoefficient` 传入实际面积，向 `SpacecraftState` 传入质量。1、250、1000、2700 kg 配置保持相同 A/m，另含双倍 A/m、零阻力面积和零光压面积。两组 LEO 初态各正反向传播 6000 s，共 28 组；NRLMSISE-00、共转 WGS84、几何太阳、球面地球双锥阴影、Cr=1.7，DP853 最大步长 1 s。TAI 作为合成 UTC，旋转地球和太阳与既有阻力协议一致，隔离外部 EOP/星历差异。

`AtmosphericDrag.SharedMassAndIndependentAreasMatchOrekitCombinedForcesAndPropagation` 使用 RKF78（最大步长 2.5 s），核对分项加速度及组合传播；容限沿用阻力协议：每分量 1e-17 m/s²、0.5 mm、5e-7 m/s。另将质量改为 9876 kg，固定各自 A/m，确认轨迹逐位相同。质量有限性、正值、乘积上溢/下溢和零面积由 `Hpop.SatelliteMass*` 覆盖。

项目 v33 / OBJS24 固定样例独立插入 1250 kg；C++/Python 精确字节往返，损坏质量即使重算 CRC 也拒绝。Python 真实 EOP/SPICE 组合力场景验证质量变更保存、计算身份、旧分析不变以及 A/m 改变后的轨迹变化；安装后 SDK 调用派生面积接口。Qt `HpopSatelliteMass*` 检查单位与键盘、430/720 逻辑宽度、不同 DPI、取消、重开和错误定位，截图路径由 `ORBITFORGE_HPOP_UI_CAPTURE` 指定。


## 海潮与组合潮汐

算法和数据约定见[海潮契约](../architecture/ocean_tides.md)。`tools/validation/orekit/OrbitForgeOceanTideReference.java` 使用固定 Orekit 13.1.2 / Hipparchus 4.0.2；所有 JAR 的 SHA-256 在同目录 `dependencies.json`。生成器校验输入 POT 原文 SHA-256，并调用库自己的 FES 势系数解析器、IERS_2010 海潮/海洋极潮、Holmes–Featherstone 和 DP853，不调用生产算法。

```text
javac -cp <jars> -d <classes> tools/validation/orekit/OrbitForgeOceanTideReference.java
java -cp <classes-and-jars> org.orekit.forces.gravity.OrbitForgeOceanTideReference <4x4-potential-path> 4
java -cp <classes-and-jars> org.orekit.forces.gravity.OrbitForgeOceanTideReference <full-potential-path> 100
```

classpath 分隔符随平台使用 `;` 或 `:`；固定输出只规范为 UTF-8 / LF，不改数值文本。输入截取步骤与完整散列见[参考数据说明](../../tests/reference/propagators/README.md)。基础 CI 不下载海潮文件，使用全部 36 个显式分潮的 4×4 固定截取。完整文件验收显式设置 `ORBITFORGE_FULL_OCEAN_FILE`，未设置时该项跳过，不能将跳过视为全模型验证。

参考使用固定 GCRF 同轴地球、TAI Epoch(0)、UTC=TAI−32 s、UT1−TAI=−31.7 s、xp=0.12″、yp=0.25″；GM=3.986004415e14 m³/s²、R=6378136.3 m。阶次包括 2/0、2/1、4/4、30/12、100/100，并分别开关海洋极潮。六个系数历元覆盖负纳秒时刻、Epoch(0)、小数秒、原版 IERS 2010 平均极点分界及其后一纳秒、较晚历元。加速度位置包括普通 LEO、近极点、GNSS 和 GEO。m=0 的 S 规范为零。

- 小文件：84 组系数、144 组总加速度、4 条轨迹。
- 完整 100×100 文件：216 组系数、240 组总加速度、12 条轨迹。
- 组合力：`OrbitForgeCombinedTideReference.java` 与 OceanTide/SolidTide 两个生成器共同编译。调用 `org.orekit.forces.gravity.OrbitForgeCombinedTideReference <4x4-potential-path>`，得到 8 条轨迹；完整固体潮不截断，海潮独立取 2/0 或 4/4。Orekit 将二者作为两个力分别计算，检验生产代码的重叠系数相加及海潮范围之外的固体潮项保留。

轨迹初值 r=(7000000,0,0) m，v=(0,6200,5300) m/s；底场 0/0，分别传播 ±6000 s 与 ±86400 s。DP853 和 RKF78 使用位置绝对容差 1e-6 m、速度 1e-9 m/s、相对 1e-14、最大步长 30 s。系数每分量容限 2e-20，加速度 2e-14 m/s²，覆盖双精度相位/累加和势函数求值舍入；轨迹容限 1 mm / 1e-6 m/s，覆盖两种积分器的全局误差。该容限不表示实际卫星预报精度。

`OceanTides.*` 另检查常相位解析系数、GM/R 缩放后的物理加速度不变、自动海洋极潮、非有限 EOP、不同分潮范围、最高 2190×0、缺项/重复/溢出拒绝和并发只读。`OceanTideFiles.*` 检查原始字节哈希、单位、坏头、取消及原子发布、固定官方身份、不可变旧分析和真实 EOP 场景/报告/存档。

Python `test_ocean_tides.py` 验证不可变易用配置、显式 DataSnapshot 与目录装配相同、原始文件变化确认、真实 EOP 传播/报告、保存重开和关闭/0/1 阶草稿身份。项目 v33 / OBJS24 使用独立 `struct.pack` 样例精确往返；非当前版本拒绝。安装后的 SDK 直接构造常相位海潮并核对解析系数。

桌面 `Hpop*Ocean*` 与 `DataCenterDialog.OceanGroup*` 检查独立阶次、文件上限、键盘、430/720 逻辑宽度、草稿重开/取消、自有下载取消/重试、排队取消不影响其他下载，以及不显示“已安装”。使用 Windows 原生 Qt、`QT_SCALE_FACTOR=1` / `1.5`，由 `ORBITFORGE_HPOP_UI_CAPTURE` 与 `ORBITFORGE_DATA_UI_CAPTURE` 保存布局截图并记录实际 DPR；同时执行相关 offscreen 回归。`DataCenterTest.*Ocean*` 另外验证后台导入、失败/取消保留；设置 `ORBITFORGE_FULL_OCEAN_FILE` 时将完整固定原文作为 loopback HTTP 响应，核验实际传输、304 条件重查及损坏替换不影响旧资源。

尚未完成完整旋转地球下的 STK 同配置轨迹、次要分潮推算和 Min. Amplitude。上述海潮测试不覆盖 EOP 日内潮汐改正或地面站海潮负荷位移。


## 文件显式引力系数长期变化

模型、导入规则和 STK 对齐边界见[长期变化契约](../architecture/gravity_secular.md)。固定 Orekit 13.1.2 / Hipparchus 4.0.2，先逐个核对 `tools/validation/orekit/dependencies.json` 中的 JAR SHA-256。生成器只使用 Orekit 的 `SecularTrendSphericalHarmonics`、`WrappingNormalizedProvider`、Holmes–Featherstone、NewtonianAttraction 和 DP853；不调用 OrbitForge。包名用于访问未修改的包级类。

```text
javac -cp <jars> -d <classes> tools/validation/orekit/OrbitForgeSecularGravityReference.java
java -cp <classes-and-jars> org.orekit.forces.gravity.potential.OrbitForgeSecularGravityReference tests/reference/propagators
```

标准输出保存为 `gravity_secular.txt`，只将换行规范为 UTF-8 / LF，不修改数值文本。组合参考需一起编译 `OrbitForgeSecularGravityReference.java`、`OrbitForgeSolidTideReference.java`、`OrbitForgeOceanTideReference.java` 和 `OrbitForgeSecularTideReference.java`；运行 `org.orekit.forces.gravity.OrbitForgeSecularTideReference tests/reference/propagators`，输出保存为 `gravity_secular_tides.txt`。classpath 分隔符在 Windows 为 `;`，其他平台为 `:`。三个输入 .grv 和输出散列见[参考说明](../../tests/reference/propagators/README.md)；生成器先校验原文字节，重复生成须逐字节一致。

三个合成 8×4 场分别使用 TT JD 2451545.0 / 2446431.0 / 2451545.123456789012345；第二个为未归一化输入。它们只用于验证公开线性模型，不代表 STK 默认 EGM 文件。Java 用 BigDecimal 解析 TT JD 并舍入到纳秒，率换算与归一化调用 Orekit；生产解析器独立实现。固定 GM=3.986004415e14 m³/s²、R=6378136.3 m、tide-free 底场和 GCRF 同轴地固系；初始 r=(7000000,0,0) m，v=(0,6200,5300) m/s。

基础参考含 48 组系数、288 组总加速度和 48 条 ±86400 s 轨迹。系数和加速度的时刻覆盖 TAI Epoch(0) 前 100 年、零点、后 26/100 年；截断 2/0、2/1、8/4，含 LEO、一般位置、近极点和高轨，以及长期变化开关两态。轨迹使用零点与后 26 年、2/0 和 8/4。两积分器最大步长 30 s、相对容差 1e-14；DP853 位置/速度绝对容差为 1e-6 m / 1e-9 m/s，RKF78 为 1e-8 m / 1e-11 m/s。每分量验收容限为系数 2e-23、加速度 2e-13 m/s²、位置 1 mm、速度 1e-6 m/s；覆盖归一化/时间换算和积分全局舍入差异，不是实际轨道预报精度。

组合参考有 8 条正反向一天轨迹。两个文件、底场 2/0 或 8/4，长期变化开启；IERS 2003 完整固体潮不截断，频率/固体极潮关闭；FES2014bv1 4×4 包含全部 36 个显式分潮和 IERS 2010 海洋极潮。UT1−TAI=−31.7 s、xp=0.12″、yp=0.25″，日月位置沿用固体潮生成器。Orekit 把底场/长期变化、固体潮、海潮作为三个独立力叠加，检验生产代码的重叠系数相加与独立截断；位置/速度沿用上述容限。

`SecularGravity.*` / `SecularFiles.*` 覆盖上述数值、最高 2190 阶率截断、百年时差、纳秒历元、非有限及溢出拒绝、重复/损坏块、导入取消、资源冻结、重准备确认及报告身份。Python `test_gravity.py` 检查只读元数据、真实 EOP 场景、保存重开、变化确认、0/0 非活动草稿和严格布尔输入；SDK 消费端调用公开率和加速度 API。项目 v33 / OBJS24 的独立固定样例与损坏测试属于本项验收。

Qt `QuantityTest.HpopSecularFileCapabilityKeyboardDraftAndValidation` 检查有/无元数据、键盘操作、错误路由、取消/重开和 0/0 草稿；用 Windows 原生 Qt、`QT_SCALE_FACTOR=1` / `1.5` 检查 430/720 逻辑宽度，并通过 `ORBITFORGE_HPOP_UI_CAPTURE` 保存实际 DPR 截图。运行日志放入新的 `out/validation/`。静态 EGM96/EGM2008 的 STK 默认演化仍待核对，尚无 STK 同配置轨迹真值。
