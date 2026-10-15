# 固体潮

HPOP 的固体潮由共享 Core 计算，桌面、Python、C++ SDK 和任务序列使用同一实现。当前支持 STK Gravity 页的 None / Permanent tide only / Full，以及 Truncate to Gravity Field Size；Full 对应关闭 Include Time Dependent Terms 的日月主要形变。频率修正、极潮、最小振幅筛选、长期变化和海洋潮仍待实现，不能将本阶段视为完整 STK 等价实现。

## 计算模型

依据 [STK Gravity](https://help.agi.com/stk/Content/hpop/hpopGravity.htm)、[IERS Conventions 2003 第六章](https://www.iers.org/fileadmin/SharedDocs/Publikationen/EN/IERS/Publications/tn/TechnNote32/tn32_057.pdf) 的公式 (1)、(2)、(12) 及表 6.1。采用非弹性地球 Love 数：k20=0.30190；k21=0.29830−0.00144i；k22=0.30102−0.00130i；3 阶前三项 0.093、m=3 为 0.094；诱导 4 阶 k2m+ 为 −0.00089、−0.00080、−0.00057。k20 使用 [STK 重力文件文档](https://help.agi.com/stk/Content/stk/importfilesGravity.htm)所述 2006 年更新后的 IERS 默认值，不推断自定义文件中的 Love 数。

日月几何地心位置在每次积分阶段转换到同一 ITRF 系，使用所选星历解的 Sun/Moon GM，与直接第三体引力的启用和 GM 覆盖独立。归一化系数为 δ(Cnm−iSnm)=knm/(2n+1) Σ[GMj/GME (R/rj)^(n+1) Pnm(sinφj) exp(−imλj)]；2 阶形变还诱导 4 阶系数。实现使用显式笛卡尔表面谐函数，日月位于极点时没有经度除零。

永久 C20 为 4.4228e−8 × (−0.31460) × 0.30190 = −4.20067548472e−9。tide_free 底场不含该项；zero_tide 底场在启用 C20 时已经包含该项。None 删除可分离的永久形变；Permanent 仅保留一次；Full 的瞬时日月响应已含永久成分，不再另加。0/0 底场没有 C20，因而不从修正中扣除不存在的底场永久项。mean_tide 当前明确拒绝，避免将直接永久潮和地球形变混为一谈。

截断开启时按静态重力的 degree 和 order 分别筛选潮汐系数；0/0 不产生潮汐力，也不要求相关星历/EOP。截断关闭时可计算至 4 阶、3 次，即使静态文件只到 2 阶、0 次，或当前选择 0/0。静态场对象保持不可变；`GravityModel::acceleration` 接收当次加性系数，既不克隆整份高阶系数表，也不改变下一次静态求值。该通用接口检查重复项、有限数、归一化索引及 360 阶计算上限。

## 接口与来源

C++ 使用 `HpopOptions::solid_tides`（`SolidTideOptions`），Python 使用 `of.orbits.SolidTideSettings(mode="full", truncate_to_gravity=False)` 并传入 `Hpop(..., solid_tides=...)`。SDK/Python 默认 None；桌面新配置默认仅永久潮，按参考截图。完整模式独立要求 Sun/Moon 星历和 EOP/Frame Provider，覆盖校验包括初始历元至全部积分路径，计算核心不联网。

模型标识 `iers2003-anelastic-primary-v1` 记录模式、截断、k20、关闭的频率/极潮、实际日月 GM 及星历/EOP 身份。项目 v23 / OBJS14 保存两个选项及非活动草稿；修改设置不改变旧分析。只接受当前格式。`.grv` 的自定义潮汐/Love 数块仍明确拒绝；没有把未读取的元数据当作默认值静默接受。

## 验证

`tools/validation/orekit/OrbitForgeSolidTideReference.java` 调用未修改的 Orekit 13.1.2 `SolidTidesField`、IERS_2003 Love 数、Holmes–Featherstone 与 Hipparchus 4.0.2 DP853。固定结果为 `tests/reference/propagators/solid_tides.txt`：120 组加速度（两种底场潮汐系统、三组日月几何、五组静态截断、潮汐截断开关、普通/近极点卫星）；四组移动日月 ±6000 s / ±1 天传播，关闭直接第三体引力。加速度逐分量容限 2e−14 m/s²（浮点球谐舍入），传播 3 mm / 3 μm/s（两种积分器误差控制差异）。另验证永久潮解析 J2、极点、低阶文件的扩展、覆盖不足、错误恢复及不可变底场。

这些是独立 IERS/Orekit 对照，尚无同配置 STK 轨迹导出实测；尤其不能把未启用的附加时间项或不同 Love 数约定的结果视为相同模型。
