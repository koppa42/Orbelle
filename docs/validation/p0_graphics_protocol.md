# 图形集成与性能验证协议

先检查 Qt/VSG/vsgQt/Rocky 的集成与生命周期，再按固定场景测量性能；单个窗口或少量对象的成功不能代替万颗卫星验收。

## 集成约束

- 保留 Core/Application、Python 与无界面 SDK 的独立构建。VSG/Rocky 类型仅进入渲染适配层和桌面装配层。
- Qt 6 Widgets 内嵌 vsgQt 原生窗口；两个视窗共享 VkDevice 和可共享场景/GPU 数据，相机及视窗状态独立。
- 地球显示使用 Rocky。地图显示数据显式指定、本地固定版本；不自动加载网络地图。
- 渲染地球固定系明确标记为 ITRF。固定圆轨道案例从 Application 取得 GCRF 结果，经 ERFA/IERS 显式旋转为显示快照；不得把 GCRF 坐标直接贴到地球固定坐标。
- 显示副本可使用 GPU 浮点格式，原始结果仍以双精度 SI 值保存，显示不回写计算结果。
- 创建、更新、视窗调整与销毁在 Qt GUI 线程调度。释放 GPU 或原生窗口前停止帧调度并等待相应 GPU 工作完成。
- 记录具体 VSG/vsgQt/Rocky/Qt/Vulkan 版本、源码散列、构建选项、设备与驱动；补丁纳入 overlay。

## 集成验证

1. 两个视窗都输出可观察的地球与卫星图像，报告实际像素尺寸及设备身份；导出本程序自身交换链图像用于检查，并独立记录实际 present 结果。若未取得桌面合成画面，不把交换链读回称为桌面截图。
2. 固定案例 6 个采样点与无界面入口相同；独立检查旋转后的快照数值与元数据，错误标签/历元明确拒绝。
3. 运行 Vulkan validation，创建、持续播放、Resize、Dock 浮动/停靠、关闭/重新创建视窗及退出过程不得出现验证错误。
4. 两视窗相机独立；检查共享的 VkDevice 和场景对象身份，不能仅凭相同类名声称共享资源。
5. 按 100%、150%、200% 缩放分别运行，记录逻辑尺寸、devicePixelRatio 和实际交换链尺寸。自动缩放测试不代替跨物理显示器移动测试，后者另行记录。
6. 冷启动、显卡/图形数据缺失、初始化失败和退出路径可定位，不能只有退出码 0 而没有实际呈现。

## 日常工作台交互分析

日常工作台的二维绘制与交互分析使用独立的小规模协议：

```powershell
python tools/validation/run_workbench_profile.py --executable out/build/desktop-graphics/apps/desktop/Release/orbitforge_desktop.exe --dependency-prefix out/dependencies/installed/x64-windows --output out/validation/workbench-24-before --satellites 24 --scale 1.5
```

在同一设备、相同采样轨迹、卫星数、窗口大小和实际 DPI 下运行修改前后的 Release 程序，每次使用新的输出目录。可再以96颗卫星比较较密集的场景。计时期间不同时构建、运行测试或启动另一图形任务；记录 `run-info.json` 中的程序散列及 `profile.json` 的实际尺寸和 `device_pixel_ratio`。`--scale` 会与系统缩放相乘，不代表最终 DPI。

比较 `map_paint_ms` 的平均值及 P95，并分别查看平移、播放、缩放和并排播放。`map_background_ms`、`map_composite_ms`、轨迹工作线程耗时用于定位开销；悬停只重绘坐标栏，不应与整幅地图绘制混在一起比较。缩放后的 `map_quality_restored_after_zoom` 必须为真。窗口隐藏、缓存更新、字体和 DPI 变化另做正确性检查。这些是 GUI CPU 计时，不能当作 GPU 帧时间、输入到屏幕延迟或万星验收结果。

## 报告曲线 CPU 对照

大数据报告使用显式诊断目标，不加入常规构建或 CTest 的耗时门槛：

```powershell
cmake --build --preset desktop-graphics --target orbitforge_report_plot_profile
$env:PATH = "$PWD/out/dependencies/installed/x64-windows/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$PWD/out/dependencies/installed/x64-windows/Qt6/plugins"
$env:QT_QPA_PLATFORM = "windows"
$env:QT_SCALE_FACTOR = "1"
out/build/desktop-graphics/apps/desktop/Release/orbitforge_report_plot_profile.exe out/validation/report-profile-before
```

固定负载为 12 个对象、各 20,000 个时刻、6 个字段（72 条曲线、2 个单位分区），包含单样本尖峰。工具测量结果重建、缩放布局与鼠标悬停加抓图的 GUI CPU 时间；缩放指标不含后续栅格绘制，抓图指标不等于真实输入到显示延迟。每项先预热，再保存样本数、平均值、中位数和 P95，同时输出全范围／局部范围 PNG。比较必须使用同一台机器、Release 配置、窗口大小与实际 DPI，并检查图片、峰值及断线没有回退；新输出目录不可覆盖。日志和程序散列另存到本次 `out/validation/`。单次采样不代表通用性能承诺，建议重复运行确认收益稳定。

## 性能验收保留项

正式性能负载仍以 `benchmarks/scenarios/windows_10k_two_views.json` 为准：10,000 颗动态卫星，两个各 1280×720 的实际渲染视窗；每视窗固定最多 100 条轨迹、200 个标签。实际轨迹细分、标签规则、地理数据/LOD、采样时长和重复次数须在测量前写入场景清单。

场景清单 schema 2 固定以下参数：100 条各 121 顶点的轨迹、每视图 200 个 GPU 标签、无影像/DEM 的 WGS84 椭球、最大 LOD 5 和 pixelError 32。每次预热 10 秒、稳定播放 60 秒、重复 3 次，分别采集 100 次缓存跳转和选中响应。预检保留标签重叠以维持固定提交量，不证明 200 个标签同时清晰可读。计时、GPU 与进程资源采样边界见[图形集成约定](../architecture/graphics.md)，运行方式见[构建说明](../development/build.md)。

稳定播放每视窗 P95 ≤33.3 ms、P99 ≤50 ms；已缓存时间跳转 P95 ≤200 ms，点击选中反馈 P95 ≤100 ms。计时必须说明是 CPU 提交、GPU 执行还是实际呈现/响应时间，不能用 Qt 定时器间隔替代帧耗时。冷启动和未命中数据单列，记录 CPU/GPU、RAM/VRAM、驱动及有效采样数量。阈值变更须明确记录并采用新的比较基线，不能通过放宽阈值掩盖失败。

正式性能结论要求两视图呈现反馈、输入关联、GPU 时间戳及资源区间均完整有效。记录尾部排空和缺失原因；CPU 完成时间不能代替不可用的输出时间。计时工具接入或短探针成功不等于正式测量通过。

CPU、GPU、内存、驱动和操作系统信息由每次运行单独记录，不写入固定工作负载。产品最低硬件配置及 Linux 桌面性能尚未验收，Linux 无界面交付检查不能代替图形验收。
