# 覆盖与重访验证

设计契约见[覆盖分析](../architecture/coverage.md)。不得用桌面显示轨迹或生产实现生成数值参考。

## 独立固定参考

`tools/validation/orekit/OrbitForgeCoverageReference.java` 使用 Orekit 13.1.2 / Hipparchus 4.0.2：KeplerianPropagator、WGS84 OneAxisEllipsoid、TopocentricFrame 与 FunctionalDetector 搜索可见区间。事件检测检查间隔 1 s、边界阈值 1e-10 s。球锥夹角通过 Hipparchus Vector3D 计算；联合覆盖通过带活动计数的事件扫描得到，区别于生产代码的排序区间合并。

面积采用 Hipparchus 8 点迭代 Gauss-Legendre 数值积分计算椭球面积元，生产实现使用解析原函数。生成器验证 `dependencies.json` 固定 jar 的 SHA-256；提交的参考与源码散列保存在 `coverage_orekit.metadata.json`。

```sh
python tools/validation/generate_coverage_reference.py --jar-dir /path/to/pinned/jars --output out/validation/coverage-reference-new
```

固定案例包括全球、区域、跨日期变更线、两个对地心圆锥传感器、两个站点跟踪传感器；每例 12000 s，两颗位于不同轨道面的倾斜椭圆轨道卫星（第二颗的初始位置与速度由第一颗绕 GCRF Z 轴旋转 1 rad 得到），最低仰角 0°/10°。坐标轴固定为不旋转的 GCRF/WGS84，隔离覆盖算法与 EOP 差异；真实 UTC/EOP 通过既有 ERFA/轨道回归及 Python/Qt 集成验证。没有本地 STK，未进行 STK 同配置实测。

C++ 自动比较全部网格中心、面积、逐点可见区间、覆盖时长、包含/忽略首尾的最长与平均空档、面积加权总统计，以及每 30 s 的瞬时/累计覆盖面积。60 s 与 30 s 检查覆盖同一独立参考。容限：网格中心 1e-14 rad；面积相对 3e-14；区间边界 2e-6 s；覆盖时长 8e-6 s；空档统计 3e-6 s；面积加权时长占比 8e-10。生产边界容差设为 1e-6 s，参考阈值更严格；时间容限计入多个根的误差累积，不放宽既有轨道精度。

另以 HPOP RKV89 点质量传播和固定科学星历采样复核同一 Orekit 全球窗口参考，边界仍要求 2e-6 s。

解析行为测试检查乱序、嵌套、重复与相接区间并集，首尾空档、始终/从未覆盖、零时长、非法网格、规模限制、取消、资源缺失、格式转换与对象失效。v46 独立 struct/zlib 二进制样例含两种覆盖报告和跨日期变更线参数，C++/Python 必须逐字节往返。

## 集成验证

Python 在独立安装环境运行完整接口回归；新增覆盖测试检查冻结快照、进度、明确资产选择、共享报告、CSV、单位转换、资源要求、取消与项目重开。`examples/python/coverage.py` 加入现有 CI 示例列表。

Qt 覆盖测试检查全球/区域设置、热力图与曲线、方向键选点、区间定位、格式切换不重新准备、模板保存、对象删除、非法输入保留旧结果及 1140/420 逻辑像素布局。使用 `ORBITFORGE_REPORT_UI_CAPTURE` 可保存实际 Windows 截图；测试于 100%/150%/200% DPI 检查。完整 Release 回归和现有整机 graphics smoke 保留；Sanitizers 仍仅手动触发。
