# HPOP 协方差局部参考系验证

本协议验证 [GCRF / STK LVLH / STK Frenet 的 Cartesian 协方差转换](../architecture/hpop_covariance.md)，包括 projected / rotating 的不同速度含义和 D/K 联合矩阵。不是 STK 实机数值验收，也不覆盖 J2000、TOD、FG、项目保存或桌面编辑器。

## 独立参考与复现

[Orekit 13.1.2](https://github.com/CS-SI/Orekit/tree/13.1.2) 与 Hipparchus 4.0.2 使用原始 `LOFType.LVLH[_INERTIAL]` / `NTW[_INERTIAL]`。STK LVLH 对应 Orekit LVLH，Frenet 对应 NTW；不能选择 `LVLH_CCSDS`。`LOF.transformFromInertial` 通过二阶自动微分求转动率，`Transform.getJacobian(USE_PV)` 输出完整 6×6 Jacobian；没有调用 Orbelle，也没有使用生产解析轴导数公式。

```sh
javac -cp "<pinned Orekit and Hipparchus jars>" -d <classes> tools/validation/orekit/OrbelleCovarianceFramesReference.java
java -cp "<classes and pinned jars>" OrbelleCovarianceFramesReference <orekit-transforms.txt>
python tools/validation/hpop_covariance_frames_reference.py --transforms <orekit-transforms.txt> --output <hpop_covariance_frames.txt>
```

脚本固定 NumPy 2.3.3、SciPy 1.16.3、mpmath 1.3.0。NumPy 直接计算 `F P Fᵀ`，`F=diag(J,I)`；mpmath 80/100 十进制位使用相同双精度输入独立核验，要求两档舍入完全一致，参考文件保存高精度舍入结果。SciPy LAPACK Cholesky 核验输入／输出活动联合矩阵。NumPy 与高精度之间的相关尺度差须小于 `3e-14`。两次完整生成须逐字节一致。

固定输入 `tests/reference/propagators/hpop_covariance_frames.txt` SHA-256：`4f2584bbf492b347ef014e03b07254deebaf8eaee4a66a725b1a720e2773100d`。其头部另记录 Orekit 原始变换文件散列。

200 组是八种轨迹几何各五种坐标表示的全部 25 种有向转换。包括圆轨、斜低轨、高偏心、逆行、极点、近径向、双曲线速度和 GEO 量级；多数案例含非中心加速度，因而轨道法向会随时间变化。无参数、仅 D、仅 K、D+K 按几何轮换，初始完整联合矩阵由固定随机种子构造，含状态、参数／状态及参数间交叉项。原始参考历元为 TAI Epoch 0；SI 单位。

Orekit 将恰好零加速度当作未提供，并使用轨道角速度近似。因此独立 Orekit 案例均有非零实际加速度；显式零加速度的生产语义另用直线运动解析值验证，不能把 Orekit 的缺省退路当成该案例的真值。

## 容限与边界

- Jacobian 左下块乘 1000 s，其余非零块不缩放；最大分量差除以 `max(1, 参考缩放最大分量)`，门槛 `3e-13`。
- 完整活动联合矩阵每项误差除以 `sqrt(Pii) sqrt(Pjj)`，门槛 `5e-13`；逆变换往返门槛 `2e-12`。未选参数的零行／列保持为零，选定参数方差及互协方差逐位保持。
- 圆轨解析验证 `Rdot` 符号、位置不确定度对旋转速度的贡献；显式零加速度的 Frenet 转动率为零。横向加速度、局部系互转、同轴 projected/rotating 互转另行验证。
- 未提供实际加速度、未知枚举、错原点／帧、非有限输入、零位置／速度及径向退化必须拒绝；GCRF STM 不可直接作用于局部系矩阵。
- 输入在取消回调前捕获；转换失败、取消和重试不发布部分结果。Python 另检查并发与返回元数据所有权。
- 完整 HPOP 以局部初始矩阵传播，与显式 GCRF 输入比较；局部系转换不改变名义轨道，前后向均验证。安装 Python 复验全部 200 组独立参考；安装 SDK 使用真实力环境的旋转 Frenet 初始协方差。

当前计算与安装接口已接入；HPOP 初始局部系的 STK 实机速度约定仍待验收。逐次最大误差、日志和构建结果保存在新的 `out/validation/` 运行目录。
