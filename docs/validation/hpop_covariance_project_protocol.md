# 项目协方差、编辑器与报告验证

本增量接通已有数值核心，不替换变分方程或联合矩阵传播算法。STK 语义依据 [HPOP Covariance](https://help.agi.com/stk/Content/hpop/hpop-covariance.htm)：初始下三角矩阵、独立重力阶次、D/K 考虑参数方差及状态/参数交叉协方差。Frame/Representation 仍按 [协方差契约](../architecture/hpop_covariance.md) 的已实现范围执行。

- 项目 v41 / OBJS32 的固定样例以 Python struct 独立添加 438 字节协方差草稿并重算两层 CRC，不调用生产写入器。C++ 与安装 Python 必须复现完整字节，错误布尔、枚举、NaN、版本和截断必须拒绝。
- 活动联合矩阵验证、禁用草稿保存、有效输入哈希、同项目快照与其他项目对象拒绝、取消可重用、并发查询、初始化资源预检进入集成测试。
- `test_hpop_covariance_project.py` 从保存重开的项目传播到既有 `universal_kepler_stm.txt` 第 14 号独立 Orekit 参考终点，用该 STM 计算 P=ΦP₀Φᵀ 与 D/K 交叉项。沿用底层对照门槛：以参考对角标准差归一化的最大矩阵误差 < 3×10⁻⁹，参数/状态交叉项绝对误差 < 10⁻⁷ SI。参考生成器和算法见[VOP 验证](vop_integration_protocol.md)及[联合协方差验证](hpop_covariance_protocol.md)。
- 新报告必须与同一 Analysis 的精确端点查询一致；平方长度单位按 10⁻⁶ 转换，表格、曲线和 CSV 共用数值，不解析显示文本计算。
- Qt offscreen 验证矩阵对称编辑、精确输入、无效矩阵拒绝、取消不提交、页签定位及缩窄保留草稿；Windows 原生平台另捕获宽/窄及不同 DPI 图像检查布局。
- 安装 Python 与 SDK 消费端运行脱离源码的公共入口；不将本地 Windows 验证称为 Linux CI 验证。

未覆盖：STK 实机同配置对照；FG/JPLFile、协方差插值、跨机动、事件跳变、三维误差椭球与其他考虑参数。报告当前每点独立积分，尚无协方差输出缓存。执行日志和截图保留在 `out/validation/`。


本轮 Windows/MSVC、Python 3.13 的项目端数值对照：最大归一化协方差误差 7.28×10⁻¹²（阈值 3×10⁻⁹）。相关项目集成 93 项、HPOP Qt 38 项、报告 Qt 13 项、安装 SDK 2 项通过；安装 Python 122 项中 120 项通过，2 项因未安装可选 NumPy/Pandas 跳过。Windows 原生 100% / 150% DPI 均检查宽/窄窗口及草稿保留。变分内核校正的额外回归见[变分传播协议](hpop_variational_protocol.md)。
