# 文档导航

首次使用请从[构建与测试](development/build.md)和[场景工作台](architecture/scenario_workbench.md)开始。功能概览见[项目首页](../README.md)，已执行的检查及其限制见[验证记录](validation/README.md)。

## 当前设计

| 主题 | 文档 |
| --- | --- |
| 场景与工作区 | [场景与属性工作流](architecture/scenario_workbench.md)、[对象与数据管理](architecture/object_data_management.md) |
| 地面站 | [WGS84 地面站与对象生命周期](architecture/ground_stations.md) |
| 项目与数据引用 | [项目文件格式](architecture/project_files.md)、[场景数据一致性](architecture/scenario_data_lock.md) |
| 数据中心 | [数据类型、下载更新、来源优先级与外推](architecture/data_center.md) |
| 报告与曲线 | [数据接口、桌面交互、STK 对照及后续阶段](architecture/reports.md) |
| 时间与参考系 | [时间尺度、EOP 与状态变换](architecture/time_frames.md) |
| 星历 | [CSPICE 内核、时间与并发约定](architecture/spice.md) |
| 图形 | [二维 / 三维集成、渲染与资源生命周期](architecture/graphics.md) |
| 模块边界 | [P0 基础类型与接口契约](architecture/p0_contracts.md) |

## 开发与验证

- [构建、测试、SDK / Python 包及便携交付](development/build.md)
- [按功能分类的验证协议与结果](validation/README.md)

验证记录保留执行时的版本、环境、结果和限制。原始日志、截图及程序包属于本地产物，不随克隆分发；复现时按对应文档重新生成。

## 早期方案

以下文档保留最初的路线选择与评审依据；其中的规划不表示功能已经实现。当前行为以代码及上方设计文档为准。

- [工程目录方案 v0.1](OrbitForge_工程目录方案_v0.1.md)
- [技术路线评审确认记录](OrbitForge_技术路线_v0.1_评审确认记录.md)
- [原始技术路线草案](OrbitForge_技术路线_v0.1.md)
