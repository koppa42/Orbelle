# Qt / VSG / Rocky 集成约定

图形工作台在 Qt 中集成 VSG 与 Rocky，数值计算和无界面 SDK 保持独立。窗口生命周期和性能测量范围见[图形验证方法](../validation/p0_graphics_protocol.md)；单星显示成功不代表万星性能达标。

可选 `graphics-benchmark` 构建使用[固定万星负载](../../benchmarks/scenarios/windows_10k_two_views.json)。场景预检与正式性能测量分别判断。

## 模块与坐标

`Visualization` 只持有独立的显示快照和显式旋转操作，不依赖图形库。`RenderVsg` 接受显式 Earth/GCRF 或 Earth/ITRF 点标记和轨迹，提供地球显示接口和编译上下文生命周期封装。每条轨迹保持同一参考系、对象和递增历元；`TrackLayer` 通过绘制范围选择缓存片段。`GeoRocky` 实现地球显示接口。Qt 工作台负责窗口、相机、播放和适配层装配。

普通工作台的三维默认 GCRF：轨迹和相机位于惯性系，地球挂在 ITRF→GCRF 旋转节点下；可切换 ITRF，二维保持 ITRF/WGS84。GCRF 拾取先把相机与标记转换到同历元 ITRF，再做 WGS84 椭球遮挡检查。三维文字标签及固定万星基准继续使用 ITRF。原始状态为双精度 SI 数据，GPU 显示副本使用浮点顶点；不接受隐式 SPICE J2000 替代 GCRF。按需缓存与坐标变换详见[场景工作流](scenario_workbench.md)。

卫星轨迹样式保存在 `OrbitDisplay`，二维与三维共用颜色、四种线型及 0.5–8 px 线宽。三维 `TrackLayer` 将每个采样段作为一个三角形实例，在顶点着色器中按该视图的 viewport 展开，处理近裁面和有限斜接，不要求 Vulkan `wideLines` 或线条点画扩展。区间变化修改实例范围，样式变化只更新小型 uniform，不重建轨道缓存和管线。虚线采用量化屏幕方向的点画坐标，避免密集采样段各自重新起笔；二维分段绘制累加屏幕距离保持虚线相位。颜色按 sRGB 保存，sRGB 帧缓冲写入前转换为线性 RGB。

当前共享样式缓冲按主窗口 DPI 和首个三维窗口的帧缓冲格式配置；同一显示缩放下的多视图与高 DPI 已验证。跨不同 DPI／色彩格式的显示器浮动窗口仍需独立的逐视图样式参数。外部三维模型材质不受轨迹颜色影响。样式持久化见[项目文件](project_files.md)。

Rocky 无影像参数时仍显示无光照椭球，供冻结基准使用；普通工作台显式传入本地 NASA 影像，加入方向光和大气边缘，不联网补地图。地球不提供分析层地形高度，显示光照与瓦片均不作为 Access 输入。

## 共享和调度

两个 Qt 原生子窗口使用同一 `vsgQt::Viewer`、`VkDevice` 和场景根节点，分别持有相机、View 和 Rocky ViewDependentState。第一个窗口实际创建设备后，将设备保存回窗口 traits；原生窗口重建时继续使用它。vsgQt 在 surface 清理时会清掉 viewer，适配类在重建时恢复该引用。

原生 surface 重建时保留逻辑 View 及其 ViewDependentState，直至后台 jobs 全部结束才释放。VSG 的延迟编译结果持有原始 View 指针，因此真正删除逻辑视图或新增阴影视图时，还需另外处理这些引用的寿命。Rocky overlay 同时修复 update 队列和编译结果的锁外读取，回调在队列解锁后执行。

Qt GUI 线程用单个定时器驱动采样、事件、更新、录制、提交和呈现。当前不启用 VSG 的多线程录制。直接检查每个 submit/present 的 Vulkan 返回值；空窗口列表和失败呈现不计入有效呈现次数。定时器间隔不是帧耗时测量。

正常播放按可见视图所在屏幕的最高刷新率安排更新，限制在 30–240 Hz，未知刷新率按 60 Hz 处理。以单调纳秒时钟累计下一次更新时刻，保留非整数毫秒周期；慢帧跳过错过的时段，不排队补帧。播放进度独立使用纳秒时间差，暂停、恢复和跳转重置基准，避免每帧截断毫秒造成时速偏差。暂停浏览、冻结图形基准及 smoke 保留原有 16 ms 调度。该调度改善更新节奏，不保证每次更新都形成实际屏幕呈现。

窗口 extent 变化后，在 acquire 和事件处理之后、CPU 投影之前调用 `RenderGraph::resized()`，让相机、视口和 renderArea 一起更新。避免标签和拾取使用旧投影、录制阶段才换成新投影；不额外重复缩放相机 aspectRatio。

VSG 1.1.15 的 early/late transfer 消费完成信号量，在每次创建提交任务后改为 TRANSFER 阶段等待，保证下一次上传开始前上次图形读取已经完成；保留真实独立上传队列。验证层在实例创建时启用，随后清除旧式设备 layer 列表，避免传入 Vulkan 已不支持的设备层配置。

## 窗口与后台编译生命周期

Rocky 会在后台编译地球瓦片。窗口重建不能直接替换或清空后台正在使用的 CompileManager，也不能保留指向旧 View 和提交任务的编译上下文。

`RenderVsg::CompileManager` 在应用存活期间保持身份不变。调整窗口前借走全部编译遍历器，等待在途编译完成和 GPU 空闲，随后清除旧 contexts。重新装配提交任务、编译场景、建立当前视图的 contexts 后归还遍历器。所有视图暂时关闭时保留屏障，重开视图后恢复。编译 hints 指定静态数据上传完成后返回，避免把旧任务里未完成的上传遗留给已经替换的任务；动态显示缓冲仍交给当前帧任务。

Qt Resize、图像获取时的交换链恢复、呈现失败后的 Resize，以及截图前等待 GPU 空闲，都使用可嵌套的短期编译屏障；它只暂借遍历器，不清除 contexts。原生窗口销毁的屏障覆盖 vsgQt 基类 cleanup，避免其 `vkDeviceWaitIdle` 与后台 `vkQueueSubmit` 并发。短期作用域退出后，只有结构重建也已完成才恢复后台编译。场景 update 在屏障外执行，因为其中可能同步调用编译器。

退出次序为：停止 Qt 帧调度 → 将 Viewer 的 ActivityStatus 置为停止，唤醒编译等待者 → 关闭并等待 Rocky jobs → 等待 GPU 空闲 → 释放提交任务、编译器、视窗、共享场景和地球上下文。初始化失败也走相同清理次序。Rocky 的 job runtime 没有直接可用的可逆暂停接口，不通过 shutdown/restart 模拟暂停。

## 验证入口

`--graphics-smoke <新输出目录>` 强制要求 Khronos validation layer。测试采用确定性历元，执行播放、缩放、Dock 浮动/停靠、隐藏/显示，并显式销毁重建两个原生窗口；随后保存本程序自己的两个交换链图像，检查有效呈现次数、共享身份、独立相机以及地球前景和卫星颜色，最后关闭所有原生窗口再退出。图像仍需人工审阅。

`RenderVsg::SwapchainCapture` 只在 smoke 指定帧使用。它先检查 surface 支持 TRANSFER_SRC；图像复制节点置于同一 CommandGraph 的 RenderGraph 之后，读取当前 acquired image，处理 COLOR_ATTACHMENT_WRITE → TRANSFER_READ、恢复 PRESENT_SRC 以及 TRANSFER_WRITE → HOST_READ 屏障。原提交的信号量覆盖复制和呈现，读回等待所属任务当帧 fence；缓冲区在等待失败时仍保留至 GPU 停止后释放。只支持明确的 8-bit RGBA/BGRA 格式，不猜测其他编码。该输出是实际交换链像素，不是桌面合成截图。

`tools/validation/run_graphics_smoke.py` 为各 DPI 比例启动独立进程，启用同步验证，检查交换链及读回像素尺寸与实际 DPR，保存退出码、日志和 JSON。它只设置子进程环境。`--rocky-resources` 可显式覆盖 shader 目录根路径，用于部署和缺资源的失败路径检查；另一个子进程将 VK_DRIVER_FILES 指向空目录验证无驱动初始化失败，不修改机器驱动。

验证层、真实 GPU、可用 Windows 桌面会话都是图形测试前提。无界面 CTest 和 Python 仍能独立运行。

## 万星固定负载

`benchmarks/graphics` 只提供渲染用的确定性圆轨道星座：100 个轨道面、每面 100 个相位。复用 Core 圆轨道解，再旋转到各 GCRF 轨道面；它与通用二体传播器的验证范围分开。通过原有 ERFA/IERS 路径预生成 0–600 秒的 601 份 ITRF 显示位置；帧间线性插值只用于显示，不向分析状态接口返回插值结果。位置缓存为 144,240,000 字节，构建时间另列。

场景共用 10,000 点的动态缓冲、100 条各 121 顶点的轨迹及单个选中环。每视图拥有 200 个 `vsg::Text` GPU 布局，字体图集和可共享资源复用。标签使用视图自己的投影和椭球遮挡筛选，按稳定 ID 顺序取前 200 个；固定基准保留重叠标签，未实现产品用的避让布局。椭球遮挡筛选属于显示功能，不是 Access 分析。

VSG 1.1.15 的动态 GPU 文字预分配 64 个字形槽，并严格限制为 1–63 个字体中存在的可打印 ASCII 字节；避免上游长度等于容量时重新分配却没有重绑已有描述符的问题。修改文字或布局后调用 `setup(64)` 标记动态数据。图集关闭默认各向异性采样，不增加当前设备未启用的 feature 要求。退出时在报告前释放字体共享缓存，纳入完整的 GPU 清理验证。

Qt 鼠标坐标乘实际 DPR 后进入屏幕空间拾取；8 像素半径内选择最近中心，等距选较小对象索引。选中环随同一显示快照更新，空白点击清除。benchmark 固定相机位置并禁用 Trackball 导航，以免鼠标操作改变测量负载；普通工作台保留相机导航。

`--benchmark-scenario` 只接受代码实现的固定场景清单及其 SHA256，不能把任意 JSON 参数误当成已支持配置。基准可执行文件额外携带固定字体、OFL 许可证和场景清单；这些不进入无界面 SDK/Python 构建。主图形示例仍通过 Application/Runtime；基准星座是独立测试夹具，不替代端到端工作台检查。

## 基准计时与资源采样

`--benchmark-run` 才启用 `BenchmarkRun`、`BenchmarkPresentation`、`BenchmarkGpu` 和 `BenchmarkMemory`。这些类仅编入可选基准桌面构建，不进入 Core、Application、SDK 或 Python。计时与交换链截图 smoke 分开运行，避免截图的同步读回影响计时。

呈现反馈使用 `(view, swapchain generation, present_id)` 明确关联，保留校准原始记录，依据实际支持的阶段输出边界命名指标。`BenchmarkGpu` 的两个 command 分别放在每个 RenderGraph 前后，在 render pass 外记录 TOP/BOTTOM 时间戳。每个视图独立持有 query pool，按提交任务的 fence 对象绑定查询槽并保留强引用。BEGIN 回调发生在 VSG `RecordAndSubmitTask::start()` 已等待并重置当前 fence 之后；先收取该槽上一代结果，再录制 reset 和本帧时间戳。仅在 begin/end 都已录制、submit 成功后登记本帧，首次使用跳过未初始化查询。最后在既有 device-idle 退出屏障后收尾，再释放图和池。

GPU 原始计数值通过独立 CSV 保存，读取使用 64 位结果与 availability，stride 16 字节，不增加 WAIT、额外 fence wait 或 queue-idle 调用。分析器使用实际 graphics queue family 的 36–64 有效位和 `timestampPeriod`，拒绝非零高位、缺失结果、错误槽位代数和不能排除多次回绕的样本。结果是每视图的 GPU 渲染图时间跨度，包含可能的流水线等待，并可能与其他视图重叠；不相加当作独占 GPU busy time。

Windows 资源采样通过 Vulkan deviceLUID 匹配 DXGI adapter，再从单一 deviceNodeMask 确定节点。`IDXGIAdapter3::QueryVideoMemoryInfo` 每秒记录当前进程 LOCAL/NON_LOCAL 用量与预算，同时记录 RAM 和进程 CPU 时间差。资源调用保存实际 QPC 包络，必须位于该帧结束与下一帧开始之间，最后样本由 `finished_qpc` 封闭；稳定段覆盖用真实采样包络计算，CPU 差值只纳入两端均在稳定段的区间。nvidia-smi 仅提供可选的整卡诊断日志。

计时 JSON schema 4 在 GPU/设备身份、资源区间与 `gpu.csv` 基础上，增加 `feedback_queries.csv`、`feedback_stages.csv`、`timing_properties.csv` 和 drain 起止/截止时间。完整返回批次先复制进采集内存，再校验和校准；原始阶段与派生时间必须一致，失败查询不解释未定义输出。刷新属性计数器按实际调用顺序核验单调性，属性只描述查询时状态。

全部输入已进入最后渲染帧并在两视图成功 present 后，停止 acquire/submit/present，在原有窗口与交换链存活期间仅轮询反馈。完整零时间报告也从 pending 移除，不能靠继续等待补造时间。全部完成即退出；否则达到两秒软截止后停止新反馈查询并记录剩余 ID，截止前的在途调用允许随后完成。GPU 查询仍在既有 device-idle 屏障后收尾。

分析器保留读取旧 schema 2/3 的能力；schema 4 另外要求原始反馈证据自洽及 drain 完成。`timing_valid` 表述呈现反馈质量，`measurements_valid` 同时要求 GPU 和进程资源数据有效，性能验收结论另行给出。原始反馈、缺失样本和准入结果均保存在当次运行报告中；完整性不满足时不得给出正式性能结论。

`--benchmark-output-only-probe` 仅用于短探针对照：只请求 FIRST_PIXEL_OUT，其他负载、轮询及联合校准不变。runner 核对实际请求掩码，应用、runner 和分析器均拒绝把该选项用于正式长测试；旧 schema 也不能绕过诊断限制。该探针不改变正式测试的默认双阶段采集方式。
