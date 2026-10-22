# 轨道事件独立数值验证

## 参考和复现

使用 Orekit 13.1.2 / Hipparchus 4.0.2 的 `KeplerianPropagator`、`ApsideDetector`、`NodeDetector` 生成参考；依赖 JAR 和 SHA256 由 `tools/validation/orekit/dependencies.json` 固定。生成器不调用 Orbelle。输入先转换成 Cartesian 并以同一双精度数值初始化双方，μ=3.986004418e14 m³/s²，地心 GCRF，TAI 秒零点 2000-01-01 12:00:00 TAI。

```powershell
python tools/validation/generate_orbital_events_reference.py --jar-dir out/validation/orekit/jars --output out/validation/orbital-events-reference
ctest --preset desktop-graphics -R OrbitalEvents --output-on-failure --no-tests=error
```

生成器使用 15 s / 7.5 s 两组参考检查间隔、1e-11 s 根容差，要求类型／数量一致、事件时刻差 ≤ 2e-8 s。记录 Java 源码、依赖和输出散列到本地 `metadata.json`。固定输入为 `tests/reference/propagators/orbital_events_orekit.txt`，SHA256 `2a66a49b2da080799b8efd16267f525990293831f7559e0378c346840db3fe37`。常规 CI 消费固定输入，不联网下载 JAR。

参考依据：[Orekit EventDetector](https://test.orekit.org/site-orekit-13.1.2/apidocs/org/orekit/propagation/events/EventDetector.html) 定义连续函数符号变化及检查间隔／根容差的区别；[NodeDetector](https://www.orekit.org/site-orekit-13.1.2/apidocs/org/orekit/propagation/events/NodeDetector.html) 明确参考面及近赤道退化。STK 设计参考见[模型契约](../architecture/orbital_events.md)。

## 场景与验收

12 个场景覆盖 LEO、GEO、Molniya 高偏心轨道、逆行、近圆、赤道、圆赤道、近逆行赤道、无事件短区间、单时刻、双曲和同时发生近地点／升交点的起点。参考包含 56 个事件（近地点 15、远地点 14、升交点 14、降交点 13）。双方排除 e ≤ 1e-8 的近远地点和 |sin(i)| ≤ 1e-8 的交点；不将未定义值算作事件。

Orbelle 分别使用 60 s / 30 s、固定网格／极值辅助两种模式，根容差 1 μs。要求每个场景的所有事件类型、数量、排序和退化统计一致，时刻误差 ≤ 1.1 μs、距离误差 ≤ 0.02 m、速率误差 ≤ 2e-5 m/s。距离和速率门槛包含事件时间不确定性引起的状态偏移，不以采样节点作为参考。当前独立对照最大差为时刻 0.456 μs、距离 0.805 mm、速率 3.86e-7 m/s。

HPOP RKV89 点质量模式另外复用同一 Orekit LEO 参考，15 s 科学星历节点、最大积分步长 30 s，位置／速度积分容差 1e-8 m / 1e-11 m/s，根容差 1 μs；事件时刻门槛 2 μs，其余同上。该检查验证科学星历到事件查询的路径，不替代摄动力模型各自的独立验证。

解析行为测试检查两端和中间的孤立零点、同时事件、常零平台、相切不穿越、无效历元／参考系／数值、零角动量、区间溢出和取消；请求之外不取值。报告集成检查多对象统计、单调进度、格式复用、Analysis 快照、项目往返和 Mission 序列拒绝。独立 v46 / RPTS7 字节夹具包含枚举 12 的轨道事件报告，与 C++/Python 写入逐字节比较。

Python 独立安装验证报告、单位、统计、取消、空结果、CSV 与项目重开。Qt 验证摘要、事件时间跳转、退化提示、删除对象后保留旧结果及窄布局；真实 Windows 100% / 150% DPI 的 1140 / 420 逻辑像素截图和运行日志写入 `out/validation/`，不进入仓库。

本协议无 STK 实机数值数据，不涵盖任意平面穿越、圈号、脉冲边界或自定义事件。检查间隔只能在声明的独立波瓣假设下发现事件，不对任意高频振荡作完备承诺。
