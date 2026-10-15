# 大气阻力验证协议

模型范围见[阻力契约](../architecture/atmospheric_drag.md)。固定参考不链接 OrbitForge，普通测试只读取已提交参考，不联网重算。尚未完成 STK 同配置数值验收。

## 密度参考

`nrlmsise00.txt` 有 396 个原作者 C 实现 `gtd7d` 参考，原始提交固定为 `bc9a2feba4344e74201281e563332688a4d09cc3`（含 32.5 km 边界修复）；SI 输出、默认变化项、日 Ap。覆盖 3 个 UTC 日期（闰日及闰秒）、11 个高度（0…1000 km）、4 个纬度/太阳时位置（含两极）和 3 组天气。

`jacchia_roberts.txt` 有 576 个未修改 NASA GMAT 标量模型参考，提交为 `c9b78bd1c8499845cb78fde315e1f761b6b06f1c`。覆盖同样日期、16 个分段高度（90…2500 km，含 100/125/200/500 km 附近）、4 个几何条件和 3 组天气。API-only stubs 不执行时间/高度换算，标量入口直接接受 UTC JD−2430000、WGS84 大地纬度、椭球高、太阳方向；未调用的外层时间和测地函数会抛异常。参考不声明等同于 GMAT 全部外层传播配置。

`nrlmsise00_ap.txt` 另含 1056 个同一原作者 C `gtd7d` 参考，覆盖 3 个日期、11 个高度、4 组几何和 8 组地磁输入：3 组原生日 Ap（含非 Kp 表节点的 15.25）、5 组历史 Ap（全零、恒定、最大值、上升和衰减磁暴）。历史模式显式设置原生 `SW[9]=-1`；测试完整密度及两个温度。历史均值包含非整数，不舍入到离散 Kp 表。参考 SHA-256 为 `da884507a21fc53dd8f5c0002bedb6270f289fbf88fbb254366b71eb219e9037`。

生成器为 `tools/validation/nrlmsise_reference.c`、`nrlmsise_ap_reference.c` 和 `jacchia_roberts_reference.cpp`；上游文件 URL 与原始字节 SHA-256 固定于 `density_sources.json`。`atmosphere_reference.py` 下载后校验字节、独立编译并在指定目录写候选文件，不能自动覆盖参考：

```powershell
# 在 MSVC developer shell 中；Linux 去掉 --msvc，使用 gcc/g++。
python tools/validation/atmosphere_reference.py out/validation/density-reference --msvc
```

Core/Python 对照密度相对容限 `2e-10`，NRL 温度绝对容限 `1e-7 K`，用于不同浮点运算顺序。另测非法日期、非法枚举、非有限参数、越界高度、Kp→Ap、极点/赤纬零/正午午夜及并发；这些测试不代表经验大气模型的物理精度。原生 Ap 测试额外检查两种输入互斥、JR 拒绝 Ap、七项长度与各项范围、显式 Ap 不读取非活动 Kp、输入数组所有权，以及日/历史模式交替和并发计算不串状态。Python 独立安装和 C++ SDK 消费端同时验证公开字段。历史 Ap 尚未用于 HPOP 轨道参考。

## 轨道参考

`drag.txt` 有 24 个独立 Orekit 13.1.2 / Hipparchus 4.0.2 `NRLMSISE00 + IsotropicDrag + DP853` 轨道和初始阻力加速度。固定 jars 及 SHA-256 复用 `tools/validation/orekit/dependencies.json`，生成器为 `OrbitForgeDragReference.java`。

参考刻意使用 TAI 作为模型的合成 UTC 时钟，初始时刻 2000-01-01 12:00:00；不混入真实闰秒/EOP 差异。WGS84 参考系绕 z 轴以 `7.292115e-5 rad/s` 转动，太阳按年周期移动并带 `0.409 rad` 倾斜。两组倾斜 LEO 初值、三组天气/物性、±3600/±6000 s，完整输入及每轴结果随行保存。Core 使用完整状态变换和 RKF78，独立参考使用 Orekit 的椭球高度、大气速度和 DP853。

DP853 最大步长 1 s，绝对位置/速度容差 `1e-8 m / 1e-11 m/s`，相对 `1e-14`；Core 最大步长 2.5 s、同样绝对/相对容差。位置每分量容限 0.5 mm、速度 0.5 µm/s；这是实现回归阈值，不是预报精度。本机参考收紧前使用 5 s 最大步长，收紧后最大位置差由 0.428 mm 降到 0.095 mm、速度差由 0.567 降到 0.125 µm/s；没有放宽测试阈值。

```powershell
$dragClasspath = (Get-ChildItem out/validation/orekit/jars/*.jar | ForEach-Object FullName) -join ';'
javac -cp $dragClasspath -d out/validation/drag tools/validation/orekit/OrbitForgeDragReference.java
java -cp "out/validation/drag;$dragClasspath" OrbitForgeDragReference
```

## 生命周期与接口

测试认证点质量+阻力仍需要 UTC/EOP/Sun，缺项和区间外查询先失败；视太阳需要额外 600 s 核覆盖，几何方向不冒充视方向；取消后 Session 可再次使用。用实际 ERFA/IERS/SPICE 2016 闰秒片段检查 C++ Session、Project、报告及 Python 状态一致；保存/重开保留参数及来源，修改 Cd/天气/密度模型/太阳方法改变新结果，旧 Analysis 保持不变。

项目 v21 固定样例独立组装，禁用参数草稿往返而不改变身份；NRL 主模型的非活动低空参数不计入计算身份。Qt 测试检查阻力开关、模型关联、面积质量比单位转换、错误定位、缺星历拒绝、取消/重开；真实 Windows 窗口检查 430/720 逻辑像素及至少两个 DPI 设置（日志记录实际 DPR），不重建编辑器、不丢失输入，操作按钮可达。


## 文件模式验收

`SpaceWeather.*` 覆盖手工可核算的 20:00 UTC 线性插值、JR/NRL 模型滞后、7 项历史 Ap、Kp 逐项转换后再平均、闰秒及跨日、内部缺日/缺字段与历史范围不足。`WeatherFiles.*` 使用官方格式示例及合成完整日期窗口，覆盖 CSV/定宽字段、缺值、重复日期、预报 Kp 中间值、段计数错误、取消/损坏后保留旧目录、原文哈希变化和旧快照不变。

轨迹验收以常量文件与手动相同模型/输入的严格一致性为基础，同时验证文件改变会影响轨迹、报告与 Session 一致、保存重开保留配置/数据引用，以及更新文件不会改变旧分析。`test_space_weather.py` 在安装包运行相同 API/持久化边界；`QuantityTest.HpopWeather*` 检查文件模式字段切换、草稿、窄窗、重开及取消；`DataCenterTest.WeatherDownload*` 经本地 HTTP 服务检查后台发布与失败回退。官方实时文件验证另记录下载时全文哈希及结果，不作为离线 CI 的联网依赖。

磁暴跳变回归在 15→400 Ap 边界比较 RKF78 最大步长 60/7/1 s 与独立固定 RK4 0.5 s，正反向 300 s 弧的末端位置差须小于 0.1 mm。该容限用于积分收敛，不代表真实大气密度误差。

## 高度近似

`AtmosphericDrag.ApproximateAltitude*` 使用独立 60 位十进制射线与 WGS84 椭球交点作为高度参考：先解 `(t*x/a)²+(t*y/a)²+(t*z/b)²=1`，再计算 `|r−t*r|`。固定点涵盖中纬度、南半球和近极点，容差 3 nm（地球半径量级 double 舍入）；验证经纬度和精确大地高没有被替换。JR/NRLMSISE 输入同一参考高度后，密度相对差需小于 `1e-12`；原作者密度网格继续独立验证模型本身。

赤道、两极及 0/90/400/1000 km 检查有限极限和域边界；在近似高度刚过 90 km、精确高度仍低于 90 km 的点，检查零宽硬切换使用正确模型；100 m 拼接区中点检查两模型等权密度；中点 Cartesian 舍入的高度容差按权重导数上界 `1.5/100 m` 传播到密度容差。非有限输入、地心及域外高度继续拒绝。

Python 检查开关、返回的实际密度高度及加速度变化；C++/Python 的保存重开、分析/报告身份、旧快照不变和 Qt 草稿/取消/重开检查均包含该开关。此处验证的是公开公式和软件路由，尚未取得 STK 同配置轨迹输出。
