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

## 文件与高阶重力

`tests/reference/propagators/gravity.txt` 是独立 Orekit 13.1.2 Holmes–Featherstone 输出，生成器为 `tools/validation/orekit/OrbitForgeGravityReference.java`，复用上述固定 jars。使用明确构造的归一化系数，覆盖 21、70、180、360 阶、全阶与带谐，以及一般位置、低轨、GEO、南北近极点，共 40 个加速度参考；每分量绝对容限 5e-12 m/s²。这个容限用于双精度不同递推顺序的数值回归，不代表模型物理精度，也不代替 EGM2008 轨道对照。

```powershell
javac -cp $classpath -d out/validation/hpop tools/validation/orekit/OrbitForgeGravityReference.java
java -cp "out/validation/hpop;$classpath" OrbitForgeGravityReference
```

`GravityFiles.*` 验证 `.grv` 归一化／非归一化、ICGEM 默认归一化、D 指数、原始换行哈希、截断／重复／NaN／不支持块、取消保留、不可变资源及项目更新确认。低层报告和场景也拒绝与项目记录不匹配的重力 Provider。`test_gravity.py` 验证目录安装、21 阶 Python 配置、准备、保存和重新打开。

官方文件另做完整下载与原文校验；不把数百 MiB 文件提交仓库或作为普通 CI 的联网依赖。可选 `DataCenterTest.LiveOfficialGravityDownload` 通过 `ORBITFORGE_LIVE_GRAVITY=egm2008` 或 `egm96` 执行真实 Qt 下载、全量安装及再次检查；未设置时跳过。桌面离线测试覆盖后台导入和失败保留、重力分组、静态有效期、360 阶计算上限与未安装模型错误。截图入口沿用 `ORBITFORGE_DATA_UI_CAPTURE` 和 `ORBITFORGE_HPOP_UI_CAPTURE`。

## 日月引力与星历

`tests/reference/propagators/third_body.txt` 是独立 Orekit 13.1.2 `ThirdBodyAttraction` / Hipparchus 4.0.2 DP853 的 32 组输出。生成器 `tools/validation/orekit/OrbitForgeThirdBodyReference.java` 使用显式定义的移动太阳/月球、点质量地球，隔离差分引力和积分的实现；不依赖 OrbitForge 力值或 STK 输出。

覆盖太阳单独、月球单独、组合、两者 GM 覆盖四种情况，LEO / GEO 初值及 ±6000 s / ±86400 s。参考每分量容限：初始第三体加速度 3e-18 m/s²，传播位置 2 mm、速度 2 µm/s。生产积分在此对照使用收紧的容差，具体数值固定在 `third_body_test.cpp`；不把该回归精度宣称为真实预报精度。另测毫米位移的潮汐极限、地心零差分、奇点、非法 GM、禁用无查询、取消后重用及覆盖先于力评估失败。

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

`SrpPersistence.*`／`SrpFiles.*` 和 `test_srp.py` 检查共享会话／分析／报告、真实 DE440s 路由、v21 保存与重开、有效参数身份及原分析不可变。桌面 `QuantityTest.HpopSrp*` 检查单位转换、有效遮挡体组合、窄窗、草稿、错误路由及取消；截图使用 `ORBITFORGE_HPOP_UI_CAPTURE`，Windows 原生平台分别设置 `QT_SCALE_FACTOR=1`／`1.5`，日志记录实际窗口 DPR。

## 集成与交付

- `HpopProject.*`：真实 ERFA/IERS 帧转换、原生 GCRF 的 EOP 身份、报告/AER 复用、初始历元覆盖与取消。
- `test_hpop.py`：Python 易用入口、不可变配置、Keplerian 的模型 μ、输出网格无关性、编辑前验证、准备结果独立和 v21 保存/打开。
- `project-current.orbitforge`：独立二进制固定输入中的 HPOP 参数精确往返；非当前版本拒绝。
- 安装后的 SDK 消费端：通过公共头文件创建 HPOP、查询并保存/重开，不依赖源码路径。
- `QuantityTest.HpopSettingsValidateCancelAndRoundTripWithoutApplyingTheOrbit`：配置草稿、阶次约束、错误校验、取消和重开；可用 `ORBITFORGE_HPOP_UI_CAPTURE` 输出宽/窄窗口截图，配合 DPI 设置检查。

运行日志与截图写入新 `out/validation/<运行名称>/`。修改力模型、系数、误差控制或分段策略时重新执行数值与交付检查；不因默认容差很小而提高产品精度宣称。

## 逐天体与 GM 来源验证

Core 的 `EveryBodyAndSourceActuallyChangesTheIntegratedForce` 将十个天体分别通过三种 GM 来源积分，并用独立的扩展精度差分公式核对速度增量（2e-14 m/s² 的分量容限），验证地心加速度项和逐阶段 Provider 路由。此短步测试验证力模型装配；长弧日月仍用已有 32 组 Orekit 参考验证。

`data/samples/spice/de440s_planets_2016_leap` 从固定原始 DE440s 保留十四条链，包括 199→1→0、299→2→0、399→3→0、301→3→0、火星与外行星系统质心→0。集成与 Python 测试检查全部天体传播、原始日月样例缺少行星时的失败、项目重开和 Cb 常数快照。CSPICE 合成断档测试覆盖 Mercury、Jupiter 的内部空隙与缺失 Saturn，不能仅查询区间端点。

使用 `compare_spice_ephemeris.py --all-planets` 对全部十四组状态与原始 DE440s 的独立 jplephem 读取比较；每分量位置容限 1 mm，速度 1e-7 m/s。固定输入、覆盖及单位说明见样例 README。每次结果写入新的 `out/validation/`；这些测试不代替相同资源及设置的 STK 轨迹比较。

大气阻力通过 `HpopOptions.drag` 接入，低层 `evaluate_atmosphere` / `evaluate_drag` 使用 SI 输入；实际 HPOP 同时保留 UTC/EOP/Sun。模型、跨接口和独立参考见[阻力契约](../architecture/atmospheric_drag.md)及[阻力验证](../validation/atmospheric_drag_protocol.md)。
