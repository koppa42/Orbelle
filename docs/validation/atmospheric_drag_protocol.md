# 大气阻力验证协议

模型范围见[阻力契约](../architecture/atmospheric_drag.md)。固定参考不链接 OrbitForge，普通测试只读取已提交参考，不联网重算。尚未完成 STK 同配置数值验收。

## 密度参考

`nrlmsise00.txt` 有 396 个原作者 C 实现 `gtd7d` 参考，原始提交固定为 `bc9a2feba4344e74201281e563332688a4d09cc3`（含 32.5 km 边界修复）；SI 输出、默认变化项、日 Ap。覆盖 3 个 UTC 日期（闰日及闰秒）、11 个高度（0…1000 km）、4 个纬度/太阳时位置（含两极）和 3 组天气。

`jacchia_roberts.txt` 有 576 个未修改 NASA GMAT 标量模型参考，提交为 `c9b78bd1c8499845cb78fde315e1f761b6b06f1c`。覆盖同样日期、16 个分段高度（90…2500 km，含 100/125/200/500 km 附近）、4 个几何条件和 3 组天气。API-only stubs 不执行时间/高度换算，标量入口直接接受 UTC JD−2430000、WGS84 大地纬度、椭球高、太阳方向；未调用的外层时间和测地函数会抛异常。参考不声明等同于 GMAT 全部外层传播配置。

生成器为 `tools/validation/nrlmsise_reference.c` 和 `jacchia_roberts_reference.cpp`；上游文件 URL 与原始字节 SHA-256 固定于 `density_sources.json`。`atmosphere_reference.py` 下载后校验字节、独立编译并在指定目录写候选文件，不能自动覆盖参考：

```powershell
# 在 MSVC developer shell 中；Linux 去掉 --msvc，使用 gcc/g++。
python tools/validation/atmosphere_reference.py out/validation/density-reference --msvc
```

Core/Python 对照密度相对容限 `2e-10`，NRL 温度绝对容限 `1e-7 K`，用于不同浮点运算顺序。另测非法日期、非法枚举、非有限参数、越界高度、Kp→Ap、极点/赤纬零/正午午夜及并发；这些测试不代表经验大气模型的物理精度。

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

项目 v19 固定样例独立组装，禁用参数草稿往返而不改变身份；NRL 主模型的非活动低空参数不计入计算身份。Qt 测试检查阻力开关、模型关联、面积质量比单位转换、错误定位、缺星历拒绝、取消/重开；真实 Windows 窗口检查 430/720 逻辑像素和 150%/225% DPR，不重建编辑器、不丢失输入，操作按钮可达。
