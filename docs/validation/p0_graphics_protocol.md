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

## 属性页任务反馈

`QuantityTest.BusyHintsEndWithoutErasingOrbitOrModelResults` 覆盖轨道计算与三维模型加载的等待提示：任务结束清除临时消息，成功、取消与错误结果在恢复控件时保留，重复恢复空闲及再次运行不会留下旧提示。

真实工作台 smoke 同时核对 HPOP 场景更新、单星更新、无修改应用、场景时段成功／取消／失败、地面站取消及 HPOP 取消后重试。完成状态必须与已发布结果及按钮状态一致，取消保留草稿和原场景，不能把其他对象的状态写成失败。显示轨迹准备取消后不残留等待状态，也不能清除其他操作的新消息。`hpop-apply-complete.png` 保存实际属性页完成状态；报告生成、过期回调与取消继续由 `orbelle_report_ui_tests` 的既有测试覆盖。

## 日常工作台交互分析

日常工作台的二维绘制与交互分析使用独立的小规模协议：

```powershell
python tools/validation/run_workbench_profile.py --executable out/build/desktop-graphics/apps/desktop/Release/orbelle_desktop.exe --dependency-prefix out/dependencies/installed/x64-windows --output out/validation/workbench-24-before --satellites 24 --scale 1.5
```

在同一设备、相同采样轨迹、卫星数、窗口大小和实际 DPI 下运行修改前后的 Release 程序，每次使用新的输出目录。可再以96颗卫星比较较密集的场景。计时期间不同时构建、运行测试或启动另一图形任务；记录 `run-info.json` 中的程序散列及 `profile.json` 的实际尺寸和 `device_pixel_ratio`。`--scale` 会与系统缩放相乘，不代表最终 DPI。

比较 `map_paint_ms` 的平均值及 P95，并分别查看平移、播放、缩放和并排播放。`map_background_ms`、`map_composite_ms`、轨迹工作线程耗时用于定位开销；悬停只重绘坐标栏，不应与整幅地图绘制混在一起比较。缩放后的 `map_quality_restored_after_zoom` 必须为真。窗口隐藏、缓存更新、字体和 DPI 变化另做正确性检查。这些是 GUI CPU 计时，不能当作 GPU 帧时间、输入到屏幕延迟或万星验收结果。

## 报告曲线 CPU 对照

大数据报告使用显式诊断目标，不加入常规构建或 CTest 的耗时门槛：

```powershell
cmake --build --preset desktop-graphics --target orbelle_report_plot_profile
$env:PATH = "$PWD/out/dependencies/installed/x64-windows/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$PWD/out/dependencies/installed/x64-windows/Qt6/plugins"
$env:QT_QPA_PLATFORM = "windows"
$env:QT_SCALE_FACTOR = "1"
out/build/desktop-graphics/apps/desktop/Release/orbelle_report_plot_profile.exe out/validation/report-profile-before
```

固定负载为 12 个对象、各 20,000 个时刻、6 个字段（72 条曲线、2 个单位分区），包含单样本尖峰。工具测量结果重建、缩放布局与鼠标悬停加抓图的 GUI CPU 时间；缩放指标不含后续栅格绘制，抓图指标不等于真实输入到显示延迟。每项先预热，再保存样本数、平均值、中位数和 P95，同时输出全范围／局部范围 PNG。比较必须使用同一台机器、Release 配置、窗口大小与实际 DPI，并检查图片、峰值及断线没有回退；新输出目录不可覆盖。日志和程序散列另存到本次 `out/validation/`。单次采样不代表通用性能承诺，建议重复运行确认收益稳定。

## 性能验收保留项

正式性能负载仍以 `benchmarks/scenarios/windows_10k_two_views.json` 为准：10,000 颗动态卫星，两个各 1280×720 的实际渲染视窗；每视窗固定最多 100 条轨迹、200 个标签。实际轨迹细分、标签规则、地理数据/LOD、采样时长和重复次数须在测量前写入场景清单。

场景清单 schema 2 固定以下参数：100 条各 121 顶点的轨迹、每视图 200 个 GPU 标签、无影像/DEM 的 WGS84 椭球、最大 LOD 5 和 pixelError 32。每次预热 10 秒、稳定播放 60 秒、重复 3 次，分别采集 100 次缓存跳转和选中响应。预检保留标签重叠以维持固定提交量，不证明 200 个标签同时清晰可读。计时、GPU 与进程资源采样边界见[图形集成约定](../architecture/graphics.md)，运行方式见[构建说明](../development/build.md)。

稳定播放每视窗 P95 ≤33.3 ms、P99 ≤50 ms；已缓存时间跳转 P95 ≤200 ms，点击选中反馈 P95 ≤100 ms。计时必须说明是 CPU 提交、GPU 执行还是实际呈现/响应时间，不能用 Qt 定时器间隔替代帧耗时。冷启动和未命中数据单列，记录 CPU/GPU、RAM/VRAM、驱动及有效采样数量。阈值变更须明确记录并采用新的比较基线，不能通过放宽阈值掩盖失败。

正式性能结论要求两视图呈现反馈、输入关联、GPU 时间戳及资源区间均完整有效。记录尾部排空和缺失原因；CPU 完成时间不能代替不可用的输出时间。计时工具接入或短探针成功不等于正式测量通过。

CPU、GPU、内存、驱动和操作系统信息由每次运行单独记录，不写入固定工作负载。产品最低硬件配置及 Linux 桌面性能尚未验收，Linux 无界面交付检查不能代替图形验收。

## 对象插入与轨道向导

Qt 控件测试 `InsertionTest.*` 检查每类对象的默认方式持久化、写入失败、连续插入防重、关闭和 Esc、传感器父对象删除、窄窗口操作可达、单颗 TLE 校验及轨道向导单位、几何与无效参数。向导圆轨道速度使用独立的二体圆轨道公式核对。

参数滑条需验证双向联动、m / km 和 rad / ° 切换、精确输入不被舍入、高轨输入自动扩展范围、隐藏草稿保留、键盘调节，以及未聚焦时滚轮不改变数值。长表单纵向滚动，主要操作始终可达，不得出现因摘要或滑条撑宽导致的横向滚动。

预览性能可显式运行 `orbelle_quantity_tests --gtest_also_run_disabled_tests --gtest_filter=*WizardPreviewInteractionProfile`。固定 2026-10-01 UTC、内置 EOP、相同 Qt 平台与窗口，分别在 1 / 3 圈下调整 RAAN 和高度；记录 12 次独立修改到地图轨迹准备完成的中位/最大耗时，再分别按 16 / 8 / 4 ms 间隔连续输入 1.6 s，统计实际交付的输入数、可绘制新轨迹次数、Qt Paint 事件中的轨迹变化次数、拖动期间最大绘制间隔及停止后的细化耗时。可设置 `ORBELLE_PREVIEW_PROFILE` 为新的输出目录，保存输入处理、交互投影和地图绘制的 CPU 分项统计。这是 Qt CPU / 轨迹缓存就绪测量，不是显示器呈现延迟，不作为 CI 时序门槛。普通回归以可阻塞的 Provider 检查高频编辑不会取消网格准备、完整作业阻塞时最新参数仍能更新、Provider 不在 GUI 线程执行、数据覆盖边界与旧结果丢弃，以及停止后 361 / 1081 个点和独立逐点计算的吻合。真实图形 smoke 同样覆盖三档输入间隔，并在 `orbit-preview-profile/profile.json` 中记录实际 DPI 和分项时间；该计数也不能解释为显示器刷新率。

真实图形 smoke 还验证默认连续插入卫星、地面站与传感器、明确父对象绑定、快捷入口遵循偏好，以及向导和 TLE 文件导入取消与确认进入属性草稿的边界。保存 `insert-objects.png`、`insert-sensor.png`、`insert-sensor-narrow.png`、九类设计的 `orbit-wizard-0.png` 至 `orbit-wizard-8.png`、`orbit-wizard-narrow.png`，按至少两个 DPI 检查参数、地图和结果区。截图必须等待对应后台地图预览完成，不能以空地图代替通过。Qt 测试另检查快速参数修改、真实 EOP 定向的初始标记、无效输入清图、1 / 3 圈切换和取消；数值约束与传播闭合见[轨道设计](../architecture/orbit_design.md)。

轨迹细化另由 `GroundMapTest.Refinement*` 检查首帧保留、异步图层完成前无空白帧、重合线段中途亮度不变、精细结果最终像素一致，以及新输入/清空中断过渡。可在 1.5 与 3 倍 DPI 重跑；真实图形 smoke 保存 `orbit-wizard-refinement-before.png`、`orbit-wizard-refinement-during.png` 与 `orbit-wizard-refinement-after.png`，记录是否捕获中间帧与图层混合 CPU 时间。完整数据就绪与图层绘制完成分开判断，滚出可见区的地图不能阻塞有效设计。
