# 验证记录与产物

此目录只保留人工整理的验证协议、结果摘要、限制和复现方法。验证脚本位于 `tools/validation/`，构建和测试入口见[构建说明](../development/build.md)。必要的固定输入与运行资源继续保存在 `data/`、`tests/`、`apps/desktop/resources/` 等对应目录。

## 场景与桌面功能

| 主题 | 记录 |
| --- | --- |
| 卫星轨迹样式与最近一次完整回归 | [2026-10-11](satellite_style_2026_10_11.md) |
| 报告与曲线 | [基础报告](reports_2026_10_11.md)、[对象范围与布局](report_ui_2026_10_11.md)、[曲线与模板](report_charts_2026_10_11.md) |
| 地面站 | [插入、编辑与计算](ground_stations_2026_10_10.md) |
| 场景与项目 | [项目文件](project_files_2026_10_10.md)、[场景名称与时段](scenario_settings_2026_10_10.md)、[对象操作与数据引用](object_data_lock_2026_10_10.md) |
| 数据中心 | [统一管理与外推](data_center_2026_10_10.md)、[安装与更新状态](data_center_management_2026_10_10.md)、[数据列表](data_inventory_2026_10_10.md)、[EOP 层级与优先级](eop_hierarchy_2026_10_10.md)、[全类型联网更新](data_center_online_2026_10_10.md) |
| 二体工作台 | [功能与便携包](p1_two_body_workbench.md) |
| 工作区与界面 | [视图与按需缓存](workspace_views_2026_10_10.md)、[界面细化](workbench_ui_2026_10_10.md)、[视觉与高 DPI](ui_refresh_2026_10_10.md) |
| 输入与交互 | [时间轴](timeline_2026_10_09.md)、[数值与单位编辑](quantity_editor.md)、[交互性能](workbench_interaction_performance.md) |

## 数值计算与交付

| 主题 | 协议与记录 |
| --- | --- |
| 基础骨架 | [Core、SDK 与 Python](p0_skeleton.md) |
| 时间与参考系 | [时间与位置变换阶段记录](p0_time_frames.md) |
| EOP 与完整状态变换 | [协议](p0_eop_state_protocol.md)、[结果](p0_eop_state.md) |
| CSPICE | [协议](p0_spice_protocol.md)、[结果](p0_spice.md) |
| 通用二体 | [协议](p1_two_body_protocol.md)、[工作台结果](p1_two_body_workbench.md) |
| J2 与初态参考系 | [结果](p1_j2_frames.md) |
| Linux 无界面交付 | [Release、Sanitizer、SDK 与 wheel](p0_linux_headless.md) |
| 计算吞吐量 | [协议](p0_compute_protocol.md)、[基线](p0_compute.md) |

## 图形验收与性能测量

| 主题 | 协议与记录 |
| --- | --- |
| 图形集成 | [验收协议](p0_graphics_protocol.md)、[生命周期与双视图](p0_graphics.md) |
| 固定万星场景 | [场景预检](p0_graphics_10k.md) |
| 计时与资源 | [呈现计时](p0_graphics_timing.md)、[GPU 与进程资源](p0_graphics_resources.md) |
| 呈现反馈 | [原始反馈与排空](p0_presentation_feedback.md)、[输出阶段受控对照](p0_output_stage_probe.md) |

以上是执行时的验证结论。图形场景预检和计时工具接入不等于性能验收通过；详细环境、数值容限与未完成项请查看原记录。

## 原始产物

日志、自动截图、逐次 JSON/CSV/XML 报告、构建缓存、依赖列表、重复源码快照和程序包属于生成产物，不提交 Git。新运行分别写入 `out/validation/<本轮目录>/` 和 `out/releases/`，不得把输出目录复制进文档目录。

2026-10-10 清理前收集的原始产物已移至本机 `out/validation/archive-2026-10-10/`，保留原有相对目录和文件内容。历史验证文档中的产物路径相对于该目录，使用代码文本标明，不作为仓库文件链接。该归档不随克隆分发；其他环境应按各协议和构建说明重新生成结果。

本次清理只取消当前版本对生成产物的跟踪，已有提交中的历史文件仍可查到。
