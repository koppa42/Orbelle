# 科学轨道星历插值

`core::make_tabulated_orbit` 与 `runtime::make_tabulated_orbit_session` 接收显式的 `OrbitEphemerisArc` 数组，拥有不可变节点副本。桌面显示轨迹不进入这个接口；它是 HPOP 积分节点输出和科学查询接入前的计算基础。当前普通 HPOP、项目分析及报告仍使用原来的直接积分，尚未自动使用本接口，也没有新增可点击的 HPOP 插值配置。

## 数学语义

按 [STK Integrator](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm) 的定义，`lagrange` 对位置和速度各分量分别插值；输出速度一般不等于位置多项式的导数。`hermite` 同时约束节点位置与速度，输出同一个位置多项式及其对 TAI 秒的导数。调用者必须提供该参考系中相对于 TAI 秒的坐标导数，不能将 TDB 速度或不同坐标系混入。

[STK Connect](https://help.agi.com/stk/SubSystems/connectCmds/Content/cmd_HPOPIntegrator.htm) 的阶数范围对应多项式次数：Lagrange 为 0–29，需要 `order+1` 点；Hermite 为奇数 3–29，需要 `(order+1)/2` 点。`OrbitInterpolationOptions` 默认 Hermite 7 是本接口的选择，不声称是 STK 默认值。VOP 插值及其 μ 尚未实现。

查询先保留左右包围节点，再按距离补齐连续窗口，距离相同优先较早节点；这样不等间隔采样也不会在局部窗口外求值。窗口在同一段内向端点移动，不减少阶数。Lagrange 0 阶按左节点保持，并将节点时刻标为间断。精确命中任一整数纳秒节点时直接返回原状态，包括输入速度。这些窗口、零阶及端点约定是 OrbitForge 的明确政策，仍需 STK 同配置实测核对。

计算先求局部 TAI 秒差，以窗口跨度归一化，避免大绝对历元损失相邻纳秒。Lagrange 用第二重心公式、权重归一化及补偿求和，并减去邻近节点的状态常数。Hermite 用合流 Newton 差商，先扣除邻近节点的直线运动，再用同一嵌套多项式同时求值及导数。无法分辨的浮点节点、病态重心权重或非有限结果明确报错。高阶并不保证高精度；调用者仍须用减半采样间隔和独立轨迹检验误差。

## 时间段与所有权

- SI 位置/速度，`Epoch` 为 int64 TAI 纳秒；全部节点必须为相同的已知原点、参考系和有限状态。
- 每段节点严格递增，所有段按开始时间严格排序；段可以共享端点，但不可重叠。共享端点采用后一段状态，表达机动后的右连续状态。
- 段间缺口和覆盖外查询抛出 `PropagationError`，不外推、不跨段补点；调用者负责在机动、非光滑力或数据边界分段。节点不足以满足指定阶数时，构造抛出 `invalid_argument`，即使查询只准备命中现有节点也不自动降阶。
- `discontinuities()` 返回段连接/缺口边界；零阶额外返回各跳变节点。当前接口不从任意节点猜测机动或生成 `ManeuverEvent`。
- 创建时校验全部输入，取消或失败不发布对象；查询无可变缓存，随机顺序及并发读取互不影响。复制进入接口的数组、原始 Session 或选项之后修改，均不影响已发布对象。结果保有查询历元和来源信息。

来源字符串记录算法版本、方法和阶数，调用者提供对象 ID 和输入修订。它不是外部节点文件的内容哈希，也不会把一份任意表接入项目的数据引用。HPOP 后续接入还需要内部节点与分段边界、参数身份、持久化、UI、报告和取消流程的完整验收。

## C++ 与 Python

C++ 经 `orbitforge/orbitforge.hpp` 使用 `OrbitInterpolationMethod`、`OrbitInterpolationOptions`、`OrbitEphemerisArc`、`make_tabulated_orbit_session`，只依赖 Runtime，不需要 CSPICE、ERFA 或 Qt。Core 工厂只依赖标准库。

```python
import orbitforge as of

options = of.OrbitInterpolationOptions()
options.method = of.OrbitInterpolationMethod.HERMITE
options.order = 3
nodes = [
    of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                      [0, 0, 0], [0, 0, 0]),
    of.CartesianState(of.Epoch(2_000_000_000), of.OriginId.EARTH, of.FrameId.GCRF,
                      [8, 0, 0], [12, 0, 0]),
]
session = of.make_tabulated_orbit_session([nodes], options, "cubic", 1)
state = session.sample([of.Epoch(1_000_000_000)]).states[0]
assert state.position_m == [1, 0, 0]
assert state.velocity_mps == [3, 0, 0]
```

示例是验证用三次多项式，不是物理地球轨道。数值参考、输入散列和误差收敛见[插值验证协议](../validation/orbit_interpolation_protocol.md)。
