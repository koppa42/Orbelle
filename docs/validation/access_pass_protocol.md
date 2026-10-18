# 可见性与过境分析独立验证

## 设计与模型对齐

界面与查询结构参考 STK [Access Options](https://help.agi.com/stk/Content/stk/tools-01a.htm)、[Computational Settings](https://help.agi.com/stk/content/stk/CompSettings.htm) 和 [Access report styles](https://help.agi.com/stkdevkit/Subsystems/dataProviders/Content/htmlStyles/Access/AccessStyles.htm)。极值辅助搜索是本项目实现，不声称复刻 STK 自适应算法或完成 STK 实机数值验收。

独立参考使用固定的 **Orekit 13.1.2 / Hipparchus 4.0.2**：KeplerianPropagator、OneAxisEllipsoid、TopocentricFrame、FunctionalDetector 的事件搜索和 BrentOptimizer 的连续极值。所有 JAR SHA-256 来自 `tools/validation/orekit/dependencies.json`，生成器不导入 OrbitForge。

为隔离事件／几何算法，参考和被测端均采用静止地球轴（GCRF 与 ITRF 轴重合、角速度为零），WGS84 椭球、GM=3.986004418e14 m³/s²、TAI 相对秒、固定站点、几何传播，关闭光行时、折射、地形。真实地球旋转与时间／EOP 由已有独立参考协议和 Access 距离变化率回归覆盖。被测端以 CSPICE 二体传播、项目的 ENU 几何、极值辅助发现与黄金分割极值计算；不使用 Orekit 代码或参考值参与计算。

## 案例与门槛

固定数据 `tests/reference/propagators/access_orekit.txt` 包含七组：多圈赤道、倾斜偏心、Nadir 圆锥（含多次短窗口）、约 18 ms 窄圆锥过境、约 98 ms 擦边过境、双边截断和空结果。Orekit 最大事件检查步长为普通场景 0.5–1 s，短窗口 1 ms，事件根精度 1e-10 s；Brent 极值绝对容差 1e-9 s。

C++ 固定回归分别用 60 s 和 30 s 最大检查间隔，边界与极值搜索容差 1 μs：

| 验证量 | 门槛与依据 |
| --- | --- |
| 过境数量、顺序、截断标志 | 完全相同，不能只比较共同找到的窗口 |
| 起止历元 | 1.1 μs，包含可见侧取边和参考舍入余量 |
| 最大仰角 | 3e-8 rad，覆盖天顶尖峰的时间误差与独立反三角函数舍入 |
| 最小斜距 | 0.01 m |
| 极值历元 | 50 μs，平坦目标函数的 binary64 舍入使极值时刻比根难辨识 |
| 总时长 | 2.2 μs × (次数+1)；逐过境两端误差可累积 |
| 平均、最短、最长时长 | 2.2 μs，按精确区间差值汇总 |

另用解析抛物线验证偏离网格、靠近网格端点的 20 ms 短窗口及可见期间的短不可见间隙；覆盖零时长接触、大负历元、非有限求值、取消及参数错误。常规 UI 默认 1 ms 容差，不能把上述严格验证配置的误差门槛当成所有默认场景的保证。

## 复现

```powershell
python tools/validation/generate_access_reference.py --jar-dir <已校验的JAR目录> --output out/validation/access-reference-new
ctest --test-dir out/build/desktop-graphics -C Release -R "integration.Access" --output-on-failure
```

生成器记录原始参考输出、源文件和依赖散列；新的输出目录不得已存在。核对新参考与固定文件一致后再考虑更新基准，禁止通过调整容限掩盖差异。Python 回归另验证拥有结果的生命周期、连续极值重新求值、报告／CSV／统计一致性、模式切换和取消。桌面测试覆盖摘要、极值跳转、窄布局保持输入与结果、过期绑定和 AER 切换；截图与逐次日志写入 `out/validation/`。

## 范围限制

最大检查间隔仍必须分辨不同峰谷；有界单峰搜索不能保证发现任意多峰、平台内尖峰或不连续事件。更复杂姿态、机动不连续点和通用约束系统不在本项中新增。统计包括分析边界截断的过境，极值是在返回的可见区间内定义；没有增加采样均值冒充连续时间平均 AER。
