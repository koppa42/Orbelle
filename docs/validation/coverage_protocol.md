# 覆盖与重访验证

设计契约见[覆盖分析](../architecture/coverage.md)。不得用桌面显示轨迹或生产实现生成数值参考。

## 独立固定参考

`tools/validation/orekit/OrbelleCoverageReference.java` 使用 Orekit 13.1.2 / Hipparchus 4.0.2：KeplerianPropagator、WGS84 OneAxisEllipsoid、TopocentricFrame 与 FunctionalDetector 搜索可见区间。事件检测检查间隔 1 s、边界阈值 1e-10 s。球锥夹角通过 Hipparchus Vector3D 计算；联合覆盖通过带活动计数的事件扫描得到，区别于生产代码的排序区间合并。

面积采用 Hipparchus 8 点迭代 Gauss-Legendre 数值积分计算椭球面积元，生产实现使用解析原函数。生成器验证 `dependencies.json` 固定 jar 的 SHA-256；提交的参考与源码散列保存在 `coverage_orekit.metadata.json`。

```sh
python tools/validation/generate_coverage_reference.py --jar-dir /path/to/pinned/jars --output out/validation/coverage-reference-new
```

固定案例包括全球、区域、跨日期变更线、两个对地心圆锥传感器、两个站点跟踪传感器；每例 12000 s，两颗位于不同轨道面的倾斜椭圆轨道卫星（第二颗的初始位置与速度由第一颗绕 GCRF Z 轴旋转 1 rad 得到），最低仰角 0°/10°。坐标轴固定为不旋转的 GCRF/WGS84，隔离覆盖算法与 EOP 差异；真实 UTC/EOP 通过既有 ERFA/轨道回归及 Python/Qt 集成验证。这套固定参考不依赖 STK；STK 实际运行对照采用下述独立协议。

C++ 自动比较全部网格中心、面积、逐点可见区间、覆盖时长、包含/忽略首尾的最长与平均空档、面积加权总统计，以及每 30 s 的瞬时/累计覆盖面积。60 s 与 30 s 检查覆盖同一独立参考。容限：网格中心 1e-14 rad；面积相对 3e-14；区间边界 2e-6 s；覆盖时长 8e-6 s；空档统计 3e-6 s；面积加权时长占比 8e-10。生产边界容差设为 1e-6 s，参考阈值更严格；时间容限计入多个根的误差累积，不放宽既有轨道精度。

另以 HPOP RKV89 点质量传播和固定科学星历采样复核同一 Orekit 全球窗口参考，边界仍要求 2e-6 s。

解析行为测试检查乱序、嵌套、重复与相接区间并集，首尾空档、始终/从未覆盖、零时长、非法网格、规模限制、取消、资源缺失、格式转换与对象失效。当前 v54 独立 struct/zlib 二进制样例含矩形、多边形、两种覆盖报告和跨日期变更线参数，C++/Python 必须逐字节往返。

## STK 实际覆盖与重访对照

`tests/reference/propagators/coverage_stk.json` 固定输入为矩形、凹多边形和跨日期变更线多边形，分别使用两颗卫星或两颗卫星上的 45° 半角对地心圆锥传感器，时段为 1 h 和 6 h，共 12 个案例。最低仰角 10°、WGS84 零椭球高、名义网格间隔 5°；关闭地形和光行时。1 h 案例包含从未覆盖点、部分覆盖及截断于分析区间边界的访问。

这是**覆盖引擎对照**，不把轨道传播、EOP 或默认网格生成差异混入访问区间误差：

- Orbelle 从当前科学 `Analysis` 导出 ITRF 位置／速度，STK 以 `Fixed` 外部星历、8 点 Lagrange 插值读取。只共享星历与网格输入，不向 STK 提供 Orbelle 的覆盖区间或指标。STK 独立计算 `CoverageDefinition` 和 `FigureOfMerit`。
- `.pnt` 包含包围矩形中的全部候选网格点，STK 使用经纬度范围或真实 `AreaTarget` 独立筛选。两边保留点集须相等；按经纬度匹配，不能依赖行号或网格点顺序。
- 每颗卫星在星历节点之间另查 360 个状态，核对 STK 实际读取的位置／速度。保存 STK 实际地球椭球参数；11.6 的极半径为 6356752.31424 m，与按 WGS84 扁率计算的值相差约 5 微米。共享地固星历隔离了 STK 默认 GM、EOP 和惯性系转换差异，不代表这些模型已经对齐。
- 导出逐点原始访问、覆盖时长、最大／平均重访空档，首尾空档分别设 Include 与 Ignore。覆盖率按时间积分的面积加权平均、曾覆盖面积比例以及每 30 s 的瞬时／累计曲线比较；重访按无任何资产覆盖的空档定义。
- 两套百分比均保留：以共同网格单元面积重新加权 STK 实际访问及 FOM，检查覆盖引擎；另用 STK 导出的原生点面积检查其原生报表。STK 即使读取自定义点位置，仍用粒度设置估算点面积，不能将这部分差异称为事件计算误差。参考[官方点集说明](https://help.agi.com/stk/Content/cov/covdef-01.htm)和[重访定义](https://help.agi.com/stk/content/cov/fom-05.htm)。

先安装当前分支的 Python wheel，再在 Windows 上运行；STK Coverage 许可是可选验证依赖，不加入普通 CI。Engine 创建自己的无图形场景，不附着用户已有窗口。每次使用新的输出目录：

```powershell
python tools/validation/compare_coverage_stk.py prepare --output out/validation/<run>/inputs
./tools/validation/measure_coverage_stk.ps1 -InputDirectory out/validation/<run>/inputs -OutputDirectory out/validation/<run>/stk -StkMajorVersion 11
python tools/validation/compare_coverage_stk.py compare --orbelle out/validation/<run>/inputs/orbelle.json --stk out/validation/<run>/stk/stk.json --output out/validation/<run>/comparison
```

基线为星历间隔 10 s、搜索间隔 30 s、双方根边界容差 1e-6 s。复核时将 `prepare` 的参数改为 `--ephemeris-step 5 --search-step 15 --boundary-tolerance 1e-8`，再运行同一 STK 和比较命令。比较两个档位的结果，确认收紧配置后差异下降，不能仅报告最有利的一次运行。该固定输入的合理检查范围是：基线区间边界／重访差异为微秒量级，精细档为几十纳秒量级；这是共同输入下的算法数值一致性，不是实际轨道或观测精度承诺。

输出保留共享输入与星历哈希、当前 Python 扩展哈希、STK 版本与实际配置、`.sc/.sa/.cv/.fm` 场景、原始数据集、逐点及时间曲线 CSV 和 `comparison.json`。比较器还独立合并多资产访问，核对 STK 覆盖时长 FOM 及原生覆盖率，避免统计脚本自身的重叠计数或权重错误。聚合器的离线回归：

```powershell
python -m unittest discover -s tools/validation/tests -p test_compare_coverage_stk.py -v
```

## 千星与密网格性能验证

`tools/validation/measure_coverage_scale.py` 使用已安装 SDK 创建确定性的 550 km、53° 倾角星座，默认区域为 20–50°N、90–130°E，最低仰角 10°，搜索步长 60 s，极值辅助搜索与 1e-6 s 边界容差。默认预热一次，再计量 `Analysis.report()`；准备场景耗时另记，`--warmups 0 --runs 1` 可测量一次冷启动报告。默认输出全部点和区间、重复运行摘要、原生模块哈希、输入／数据哈希及 Windows 进程峰值工作集。密网格可用 `--summary-only` 逐点计算摘要，避免验证脚本额外保留百万点 Python 列表；计算结果仍完整。`--memory-mib` 设置任务预算，峰值工作集则包含 Python、场景、结果及分配器开销，两者口径不同。

```powershell
python tools/validation/measure_coverage_scale.py --satellites 1024 --resolution 5 --output out/validation/<run>/1024
python tools/validation/measure_coverage_scale.py --satellites 2048 --resolution 5 --runs 1 --output out/validation/<run>/2048
python tools/validation/measure_coverage_scale.py --satellites 1024 --resolution 5 --cone 45 --duration 600 --runs 1 --output out/validation/<run>/sensors
python tools/validation/measure_coverage_scale.py --satellites 1 --global-grid --resolution 1 --duration 10 --runs 1 --output out/validation/<run>/global
python tools/validation/measure_coverage_scale.py --satellites 1 --global-grid --resolution 0.2 --duration 10 --runs 1 --summary-only --memory-mib 1536 --output out/validation/<run>/million-points
python tools/validation/measure_coverage_scale.py --satellites 1024 --resolution 5 --cone 45 --duration 3600 --runs 1 --output out/validation/<run>/sensors-hour
```

长时段须指定覆盖完整日期范围的 EOP 快照。`--start-date` 按 UTC 零时解释，`--eop-manifest` 明确数据输入；默认的 2000 年两日快照不适合直接延长为一周。以下一天／一周配置使用同一轨道、区域和搜索参数，数据范围为 2026-09-01 至 2026-11-01；可加 `--progress` 查看计算进度。计时任务串行运行，未完成或手动终止的尝试单独记录，不将其当作完成耗时。

```powershell
python tools/validation/measure_coverage_scale.py --satellites 1024 --resolution 5 --duration 86400 --runs 1 --warmups 0 --start-date 2026-10-01 --eop-manifest data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json --progress --output out/validation/<run>/satellites-day
python tools/validation/measure_coverage_scale.py --satellites 1024 --resolution 5 --duration 604800 --runs 1 --warmups 0 --start-date 2026-10-01 --eop-manifest data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json --progress --output out/validation/<run>/satellites-week
```

缓存与资产排序优化后的首轮 Windows Release 实测（时间不含准备和结果导出；峰值工作集包含 Python、场景及结果）：

| 资产 | 点数 | 时段 | 预热后报告耗时 | 峰值工作集 |
|---|---:|---:|---:|---:|
| 32 卫星 | 15 | 1 h | 0.459 s，3 次中位数 | 55 MiB |
| 1024 卫星 | 49 | 1 h | 2.243 s，3 次中位数 | 79 MiB |
| 2048 卫星 | 49 | 1 h | 2.352 s，1 次 | 96 MiB |
| 1024 个 45° 半角传感器 | 49 | 10 min | 4.658 s，1 次 | 93 MiB |
| 1 卫星，全球 1° | 41344 | 10 s | 0.233 s，1 次 | 113 MiB |

相同的 32 星输入在优化前中位数为 1.058 s；优化后的所有点、面积、覆盖时长、重访和区间边界与之前逐值相等。首轮实现中的 1024 个 45° 半角传感器、49 点、1 h 场景曾触及 5000 万次几何评估上限。用空间／时间分批、有界缓存和可调整存储预算替代累计工作量与固定点数限制后，同机安装 wheel 的该轮复测如下：

| 资产与网格 | 点数 | 时段 | 任务预算 | 预热后报告耗时 | 进程峰值工作集 |
|---|---:|---:|---:|---:|---:|
| 32 卫星 | 15 | 1 h | 512 MiB | 0.469 s，3 次中位数 | 55 MiB |
| 1 卫星，全球 0.5° | 165188 | 10 s | 512 MiB | 0.898 s，1 次 | 144 MiB |
| 1 卫星，全球 0.2° | 1031760 | 10 s | 1536 MiB | 5.544 s，1 次 | 739 MiB |
| 1024 个 45° 半角传感器 | 49 | 1 h | 512 MiB | 30.310 s，1 次 | 209 MiB |

32 星复测与最初基线逐点严格相等。百万点案例验证完整结果的存储和计算，不代表百万点与千星、长时段组合的耗时。性能与轨道分布、视场、分析时段及搜索设置有关，不能按资产数或点数简单外推。完整结果仍驻留内存，任务预算不足时明确失败；网格密度由分析需求与可用资源决定，而非固定点数上限。

该轮安装 wheel 另重跑了实际 STK Engine 11.6 的 12 例、576 点对照：区间数量全部一致，共同面积权重下平均覆盖率最大差异 1.0803e-8 个百分点，逐点最长重访空档最大差异 1.654e-6 s。STK 原生点面积导致的平均覆盖率差异最大约 0.1142 个百分点，仍单独报告；不能混入引擎数值误差。

长时段复测发现逐点扫描整个时段会导致缓存反复失效：1024 星、49 点的一天报告运行 408.2 s 后仍未完成，停止时进程峰值工作集 106 MiB。随后改为在同一时间窗口完成点集／资产并集，再推进时间，保留原搜索格点和求根容差。2026-10-25 Windows Release 安装 wheel 实测（2026-10-01 UTC 起算、相同 EOP、512 MiB 任务预算、每例一次冷启动报告）：

| 资产 | 点数 | 分析时段 | 报告耗时 | 进程峰值工作集 |
|---|---:|---:|---:|---:|
| 1024 卫星 | 49 | 1 h | 2.150 s | 82 MiB |
| 1024 卫星 | 49 | 1 d | 56.891 s | 95 MiB |
| 1024 卫星 | 49 | 1 week | 350.982 s | 100 MiB |
| 1024 个 45° 半角传感器 | 49 | 1 h | 32.540 s | 213 MiB |
| 8 个 45° 半角传感器 | 49 | 1 d | 15.453 s | 62 MiB |
| 8 个 45° 半角传感器 | 49 | 1 week | 106.009 s | 63 MiB |

这组密集星座在所选区域达到全时段联合覆盖，算法可跳过已有并集覆盖的时段，因此不代表窄视场传感器的长时段耗时；千星传感器的完整一天／一周尚未实测。八星传感器一周的平均覆盖率约 2.2026%，最长空档约 27702.188 s，覆盖了稀疏访问与跨日空档；将一周结果裁剪至首日后，49 点的访问区间与单独计算一天逐纳秒一致。分析时段需要包含在场景与科学轨迹的有效范围内；起止日期、检查间隔、仰角和网格密度均为明确输入，不因时间跨度增加而自动放宽。

时间窗口重排后，19 项相关 C++ 测试、24 项报告／热力图 Qt 测试和已安装 SDK 的 2 项覆盖接口测试通过。实际重跑 STK 的既有 1 h／6 h、12 案例对照，Orbelle 逐点结果与优化前严格相等，区间数量无差异；共同权重平均覆盖率最大差异仍为 1.0803e-8 个百分点，最长重访空档最大差异仍为 1.654e-6 s。该 STK 对照未扩展至一周；长时段基准与短时段引擎精度验证分别报告。

新增回归检查 2048 资产并集、完整覆盖后的输入校验、内存预算不足、运行中取消、跨资产精确历元坐标变换复用，以及密网格的 WGS84 面积守恒、流式多边形筛选和浮点表示边界。超过缓存容量的 60001 个时刻仍共享跨资产变换，避免长时段反复计算同一变换；访问区间的起止截断标记以完整分析时段为准。完整计算超过 16 万个网格点与 20 万个输出时间节点，逐节点核对跨批次、末尾不足整步长时的区间成员关系。界面验证后台估算取消、快速连续修改设置及调整预算；热力图比较缓存绘制和全新控件，覆盖选点、指标切换、缩放及清空结果。

## 集成验证

Python 在独立安装环境运行完整接口回归；新增覆盖测试检查冻结快照、进度、明确资产选择、共享报告、CSV、单位转换、资源要求、取消与项目重开。`examples/python/coverage.py` 加入现有 CI 示例列表。

Qt 覆盖测试检查全球/区域设置、热力图与曲线、方向键选点、区间定位、格式切换不重新准备、模板保存、对象删除、非法输入保留旧结果及 1140/420 逻辑像素布局。使用 `ORBELLE_REPORT_UI_CAPTURE` 可保存实际 Windows 截图；测试于 100%/150%/200% DPI 检查。完整 Release 回归和现有整机 graphics smoke 保留；Sanitizers 仍仅手动触发。
