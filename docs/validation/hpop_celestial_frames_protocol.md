# HPOP 天文系与联合协方差验证

## 模型与来源

`fk5_j2000` / `true_of_date` 与 GCRF、SPICE J2000 分别定义。
[STK 参考系设计](https://help.agi.com/stk/Content/stk/referenceFramesCentralBodies.htm)与
[AGI Earth Definition](https://help.agi.com/odtk/content/od/ODObjectsScenarioEarthDefinition.htm)
说明 ICRF↔J2000 经 pseudo-fixed 的两条路径实现，且 FK5 章动模式影响结果。
本实现选择 STK 支持的 IERS1996；**不声称等于默认 JPLFile/DE405**。

- 现代路径：已有 ERFA 2.0.1 IAU2006/2000A，模型 s、观测 dX/dY、ERA00。
- FK5 路径：IERS1996 Table 5.1 的全部 106 项 IAU1980 章动及 1996 基本角；
  岁差采用 Lieske 的 psi_A / omega_A / chi_A 表示；GMST82 + 分点方程。
- `TOD = Rz(ERA − GMST82 − EqEq) × C2I`；
  `J2000 = (N80 × P76)ᵀ × TOD`。不引入极移或 TIO locator s′。
- 时间为 TAI 纳秒；TT 用 TAI+32.184 s；UT1 用显式 EOP 的 UT1−TAI。
  快地球自转项在 ERA−GMST82 中代数抵消，避免大角度相减。
- IAU1994 C7 的两个补充项从 1997-02-27 00:00:30 TAI 启用。
  该精确模型切换时刻支持位置/projected，拒绝时间导数/rotating；
  相邻纳秒取各自模型侧，差分不跨侧。
- 完整速度使用 Rdot；四阶中心差分仅作用于慢旋转及该历元 EOP 的局部线性延伸。
  步长 32 s；观测 CPO 的变化率另对 CIP 矩阵解析求导，避免大 CPO 变化率迫使差分步长过小而丢失精度。
  输出导数投影到旋转矩阵的切空间，移除舍入的非旋转分量。
  不额外查询 EOP，因此允许数据覆盖端点。
- 接口接受的有限 EOP 数值和变化率范围沿用 ERFA Provider；
  固定对照跨度为 1972–2050，不据此承诺该范围外同等实测精度。

数字表来源是 [Orekit 13.1.2 IERS1996 数据](https://github.com/CS-SI/Orekit/tree/13.1.2/src/main/resources/assets/org/orekit/IERS-conventions/1996)，
其说明为从 IERS Technical Note 21 原始 TeX 提取数值。
`tab5.1.txt` 原始 SHA-256 为
`584c2fd400a65495c49c396b62d6c80199564219e11a72f94b2db41b1199632c`；
`nutation-arguments.txt` 为
`7a982277d9fc373d2978cc74d6a78552c26d58807ed557f31e21e3943205b1ea`。
生产代码独立求和，不复制 Orekit 算法实现；其中 Omega 的线性项是 −6962890.2665″/century，
不能换成 IERS2003 的值。

## 独立算法与容限

`OrbelleCelestialFramesReference.java` 使用固定 Orekit 13.1.2 / Hipparchus 4.0.2
的 IERS1996 基本角、章动、岁差函数及旋转类；现代路径采用独立 IERS2010 X/Y/s 级数。
Hipparchus 自动微分给出时间导数，参考不使用有限差分，也不加载 Orbelle/ERFA。
ERA−GMST 的值用 80 位 Dfp 运算防止相减损失；导数来自 Orekit 的 Field 函数，
另显式计入 UT1−TAI 变化率（Orekit GMST Field 函数将外部 offset 按常量取样）。

48 组包括两种天文系、零/非零极移及 CPO、CPO/UT1 变化率、1972/2000/2016/2026/2050、
C7 两侧的秒/纳秒以及一个接近接口上限的合成 CPO 变化率。
三种误差分别检查：

| 对照 | 绝对容限 | 目的 |
| --- | --- | --- |
| 整条旋转矩阵 | 2e−11 | 沿用现代参考系 Orekit 对照门槛，包含不同 IAU2006/2000A 级数表示 |
| 独立 FK5 因子，与相同 ERFA C2I 组合 | 2e−12 | 隔离新增 IERS1996 链，不把现代 CIP 级数差异计入 |
| 完整 Rdot | 2e−15 s⁻¹ | 与自动微分核对，包含 EOP 变化率 |

`hpop_celestial_covariance_reference.py` 从旋转及 Rdot 生成 96 组
projected/rotating 的 8×8 完整联合矩阵，均包含 D、K 与交叉项。
NumPy 直接计算 FPFᵀ，mpmath 80/100 位舍入结果必须一致，SciPy Cholesky 检查正定。
NumPy/高精度相关归一化误差限 3e−14；C++ 与 Orekit/高精度结果限 1e−10，
包含上表角度差异和位置/速度尺度放大；往返限 2e−13。
D/K 方差和互协方差必须逐位保持。

额外检查覆盖：FrameId 与元数据、GCRF/ITRF 路由、速度往返、极移抵消、
错误输入、缺少 Provider、切换时刻、只有一个可用历元的 EOP、
回调期间替换环境、独立安装 Python 的前后向传播与并发、实际安装 SDK 调用。
参考矩阵文件位于 `tests/reference/time_frames/` 与 `tests/reference/propagators/`；
测试不联网。

## 重现

准备 `tools/validation/orekit/dependencies.json` 中所有 JAR（生成器验证 SHA-256），
Java 21，以及 Python 的 NumPy 2.3.3、SciPy 1.16.3、mpmath 1.3.0：

```text
python tools/validation/generate_celestial_frames_reference.py --jar-dir <jars> --work-dir out/validation/celestial/classes --frames-output out/validation/celestial/frames.txt --covariance-output out/validation/celestial/covariance.txt
cmake --build --preset desktop-graphics
ctest --preset desktop-graphics -R "erfa.|core.HpopCovariance"
```

将生成结果与固定文件逐字节比较。独立安装后运行
`test_hpop_celestial_frames.py`、既有协方差/变分/时间参考系测试和 SDK consumer。
完整运行证据写入新的 `out/validation/` 子目录。
项目矩阵保存、桌面编辑及 Analysis/报告接入由[项目验证协议](hpop_covariance_project_protocol.md)覆盖。STK 实机、默认 JPL 章动文件及 FG 仍按[延期范围](../architecture/hpop_alignment.md#延期范围)处理。

## 固定参考身份与已测误差

- 天文旋转文件 SHA-256：`dc007d00823a031a62940e2ea5f79b78cd7cf958a080a95fa09d622fce3583d1`。
- 联合协方差文件 SHA-256：`60d0e2ddb8caa7fcc65fa36c39b34d8052e73d6ce9558d15798fab142211c095`。
- 两次完整参考生成的原始字节一致；NumPy/高精度最大相关尺度差 3.784e−16。
- Windows/MSVC 实测：完整旋转最大元素差 4.461e−12，隔离 FK5 因子差 4.987e−15，
  Rdot 差 1.742e−15 s⁻¹；完整联合矩阵相关尺度差 9.829e−12。
  包含最大允许数量级的合成 CPO 变化率；未放宽上表门槛。
