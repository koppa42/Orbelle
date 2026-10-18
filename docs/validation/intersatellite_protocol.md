# 星间分析验证协议

## 独立参考

`tools/validation/orekit/OrbitForgeIntersatelliteReference.java` 不调用 OrbitForge：使用固定 Orekit 13.1.2 / Hipparchus 4.0.2，双 KeplerianPropagator、LOFType.QSW、InterSatDirectViewDetector（零掠过高度），以及 BrentOptimizer 连续极值。依赖与 SHA-256 由既有 `dependencies.json` 固定；参考数据和 Java 源散列保存于 `tests/reference/propagators/intersatellite_orekit.metadata.json`。

参考采用地心惯性轴与静止 WGS84 椭球，隔离参考系模型差异；实际 ITRF 转换另走已验证的 IERS/ERFA 链，并由真实 EOP 的冻结 Analysis/低层报告一致性、Python 及桌面测试验证接线。不能把静止椭球参考称为真实地球姿态的端到端独立对照。

```powershell
python tools/validation/generate_intersatellite_reference.py --jar-dir <固定 JAR 目录> --output <新输出目录>
```

九组情形覆盖同轨道面、不同轨道面、逆行、LEO–GEO、极轨、全程可见、全程遮挡、端点极值及单历元。采样间隔为 137 s，搜索分别使用 60/30 s、固定网格/极值辅助；Orekit 事件最大检查间隔 5 s、时间阈值 1e-10 s，Brent 独立按 20 s 子区间优化。两体 GM=3.986004418e14 m³/s²，参考初态直接以 SI Cartesian 保存。

## 门槛

| 量 | 验收门槛 | 理由 |
| --- | --- | --- |
| 采样距离 / RIC 分量 | 0.0002 m | 独立传播与轴变换对照 |
| 距离变化率 / 惯性相对速率 | 3e-7 m/s | 独立速度与投影 |
| 可见性边界 | 1.1e-6 s | 查询设定 1 us，加传播/舍入余量 |
| 连续全局极值 / 窗口内部极值距离 | 0.0002 m | 包含端点，全局极值直接比较 |
| 可见窗口边界上的最近距离 | 0.02 m | 边界 1 us 乘最大星间速率的误差传播；不用于内部极值 |
| 非平坦曲线极值时刻 | 0.003 s | 距离极值附近双精度平坦区；不把时间搜索阈值误当成物理时刻精度 |
| HPOP 固定科学星历 | 0.02 m / 2e-5 m/s；遮挡边界 2 us | 同 GM、点质量 RKV89、1e-14 相对容差、最大步长 30 s、固定输出 15 s |

平坦等距轨道只比较极值距离，不比较无唯一解的极值时刻。零距离、径向退化、相切、极半径、错误历元/坐标系、非有限输入、重复/缺失对象、缺少 EOP、取消、模板重排与失败后旧结果保留另做行为与边界测试。

## 验证入口

- `integration.Intersatellite.*`：独立参考、步长减半、两种搜索、HPOP、资源身份、取消和退化输入。
- `ReportDialog.IntersatellitePairCurvesExtremaSeekAndResponsiveLayout`：对象顺序、双击/最近时刻定位、曲线、单位复用、420/1140 逻辑像素、删除/重复绑定保留结果。
- Python `test_intersatellite.py`：有序绑定、冻结快照、统计只读、CSV/模板、保存重开、单历元与零距离；固定格式夹具覆盖两种新报告。
- 完整 Release CTest、独立 Python 3.13 API 和十一项示例；PR CI 保留 Linux/Windows 与多 Python 版本。ASan/UBSan 仍仅手动运行。

本次 Windows / MSVC 19.51 Release、Python 3.13 初步结果：采样距离/RIC 最大误差 3.46452e-7 m，速率 5.05679e-10 m/s，可见性边界 8.22944e-7 s，极值时刻 2.40822e-4 s；HPOP 独立对照通过上述门槛。命令、完整回归日志和截图记录在 `out/validation/intersatellite-*`。STK 仅用于定义核对，尚无 STK 同配置数值实测。
