# 光照与地影验证协议

## 独立参考与模型对齐

事件参考使用 Orekit 13.1.2 `EclipseDetector`、`KeplerianPropagator` 和 `SolarRadiationPressure.getLightingRatio()`，Hipparchus 4.0.2；JAR 的 SHA-256 固定在 `tools/validation/orekit/dependencies.json`。生成器不导入 OrbitForge。

采用地球球面 6378137 m、太阳半径 695700000 m、GM 3.986004418e14 m³/s²，固定几何太阳，GCRF / SI，TAI 零点为 2000-01-01 12:00:00。输入太阳方向、轨道初态和时间均写入固定参考文件。固定太阳用于隔离事件与几何算法，真实 DE440s 的加载、覆盖、数据锁、保存/重开另由 Python/C++ 集成验证。

九组案例包含低轨、偏心倾斜轨道、GEO、全光照、查询全在本影、单时刻、环食、约 0.393 s 的擦边阴影、约 0.388 s 的短本影。Orekit 事件阈值 1e-10 s，普通参考检查间隔 1/2 s，短事件 0.001 s；每组再将参考间隔减半，事件数量、状态和时刻须在 1e-8 s 内收敛。光照比例采样覆盖每种状态内部的 10%、50%、90% 和查询端点，时刻先对齐整数纳秒，并保存独立传播状态。

## 精度和边界判据

OrbitForge 分别使用 60 s 和 30 s 最大检查间隔、1 μs 根容差。须得到相同数量与类型的完整连续分段，边界绝对误差 ≤1.1 μs，最长时长误差 ≤2.2 μs，各状态累计时长误差 ≤2.2 μs ×（段数+1）；占比误差按累计时长预算除以查询跨度。所有时段之和须等于查询时长（1 ns 数值检查），clipped 标志须与参考一致。

Orekit 双精度圆盘面积公式在极浅接触处发生相减消减。为避免把这一差异当作产品精度标准，另以 **mpmath 1.3.0、80 位精度**计算独立球面视角及圆盘交集面积，并对同一参考状态生成固定比例。实现采用短弧圆缺面积的级数展开改善消减，仍与 SRP 共用几何。每个实际传播样本相对高精度参考须 ≤5e-11；Orekit 比例仍逐项核对，审计后的包络为普通案例 3e-9、短本影 3e-8、极浅擦边 2.5e-7。这三个包络约束参考算法差异，不放宽高精度验收。环食另检查同心解析面积与分类。

## 复现

```powershell
python -m pip install mpmath==1.3.0
python tools/validation/generate_lighting_reference.py --jar-dir <verified-jars> --output out/validation/lighting-reference
ctest --test-dir out/build/desktop-graphics -C Release -R Lighting --output-on-failure
```

固定文件：`tests/reference/propagators/lighting_orekit.txt`，SHA-256 `bd574b6518b749120dd329bd55026d3605fa5fed8ee206ca74cf82dd2acf4ccc`。生成元数据包含 Java/高精度脚本和固定结果散列；例行 CI 读取固定值，无需 Java/mpmath 或联网。JUnit `maximum_reference_errors` 记录本次误差。

此外验证无太阳资源、身份不匹配、覆盖不足、非法区间/步长、非有限几何、取消、快照不可变、多卫星统计、显示模板重用、CSV、v44 保存及 Python 独立安装。Qt 检查摘要、曲线、时刻跳转、失效对象和宽窄布局，并在 Windows 100%/150% DPI 保存实际截图。共用 SRP 改动须运行既有 SRP 数值与导数回归。

## 限制与来源

当前仅验证地球球面与几何太阳，无本机 STK 对照数据，不声称 STK 全部模型等价。检查间隔必须分辨独立事件波瓣，根容差不能弥补遗漏搜索。无月球、地形、折射或光行时模型。

- [STK Lighting Times](https://help.agi.com/stkEngineOnUNIX/Subsystems/dataProviders/Content/html/dataProviders/Lighting_Times~Vehicle.htm)
- [STK Lighting settings](https://help.agi.com/stk/content/stk/vehSat_eclipseBodiesTab.htm)
- [Orekit EclipseDetector](https://github.com/CS-SI/Orekit/blob/13.1.2/src/main/java/org/orekit/propagation/events/EclipseDetector.java)
- [Orekit SolarRadiationPressure](https://github.com/CS-SI/Orekit/blob/13.1.2/src/main/java/org/orekit/forces/radiation/SolarRadiationPressure.java)
