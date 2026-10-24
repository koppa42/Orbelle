# 地面站与 Facility 基本属性

通过“插入 → 插入对象 → 地面站”创建固定于地球的设施。Facility 沿用已有地面站对象、稳定 ID、Access/AER 与传感器指向关系。选择“定义属性”时先编辑草稿，点击“插入地面站”生效；“使用默认参数”创建经纬度、地面椭球高与离地高度均为零的站点。

属性页分为“基本 → 位置 / 描述”和“约束 → 基本”。宽面板使用左侧树，较窄面板使用页签或页面选择框；各页内部滚动，操作按钮固定在底部。位置页集中显示名称、坐标与离地高度；WGS84 基准在分组标题旁标注，解释文字通过帮助按钮查看，设备位置预览默认收起。切页、缩放和调整面板宽度不重建输入控件，保留草稿、单位与页面选择。重置恢复上次应用值，关闭不会提交草稿。描述为最多 16 KiB 的 UTF-8 纯文本，可包含换行。

## 位置与高度

坐标定义**地面点**；设备位置沿该点的 WGS84 法线叠加离地高度。东经和北纬为正，公共计算 API 使用弧度和米。

| 坐标类型 | `ground_station_coordinates` / `ground_station_location` 的三个值（SI） |
| --- | --- |
| `geodetic` / `GEODETIC` | 大地纬度、经度、地面椭球高 |
| `geocentric` / `GEOCENTRIC` | 地面点在椭球上法线投影点的地心纬度、经度、地面椭球高 |
| `cartesian` / `CARTESIAN` | 地面点的地心 ITRF X、Y、Z |
| `spherical` / `SPHERICAL` | 地面点位置向量的球面纬度、经度、地心距离 |
| `cylindrical` / `CYLINDRICAL` | 地面点距极轴的距离、经度、ITRF Z |

地心纬度按 STK 的椭球投影点约定解释；地心坐标中的高度仍沿椭球法线量取。有高度时，它不等同于球坐标的位置向量纬度。参考 [STK Facility Position](https://help.agi.com/stk/Content/stk/fac-01.htm)。UI 坐标类型切换转换当前有效草稿；无效输入阻止切换并保留原文字。仅切换坐标表示与单位保留精确的大地坐标，不将数值往返误差写回物理输入。

- 经度范围 ±180°，纬度范围 ±90°；与 STK 接受 ±360° 的输入范围有差异。极轴上的直角坐标逆变换约定经度为 0；球/柱坐标保留传入的经度。
- 地面椭球高支持 −12,000 至 100,000 m，离地高度支持 0 至 100,000 m，设备总椭球高不得超过 100,000 m。非有限值、地心内部不可用坐标及越界输入均明确拒绝。
- 高度基准固定为 WGS84。椭球高不是海拔；当前不接入 MSL、大地水准面或地形服务，也不提供会暗示这些功能可用的选项。
- WGS84 半长轴 6,378,137 m、扁率 1 / 298.257223563，与 [ERFA WGS84 参数](https://github.com/liberfa/erfa/blob/master/src/eform.c)一致。
- `GroundStationLocation.height_m` 是地面椭球高，`height_above_ground_m` 是设备离地高度。`ground_station_position` 和 `core::GroundStation` 返回设备的实际位置；坐标表示转换排除离地高度。
- `core::GroundStation` 是地心 ITRF 状态提供者，ITRF 速度为零，历元保持请求的纳秒值。不含板块运动、固体潮、海潮负荷或站点实测速度。转为 GCRF 必须使用完整状态变换，包含地球旋转速度项。

## 可见性与显示

AER 从设备位置计算，仰角相对于当地椭球法线确定的水平面。最低仰角支持 −90° 至 90°，默认 0°；可见性还必须满足地面遮挡。离地高度大于 1 mm 时可向下看到当地地面，负的最低仰角不会绕过地面遮挡。

地面遮挡使用地心对称、经过站点地面点且具有相同法线的扁椭球，而非总是使用零高 WGS84。令大地纬度为 φ、地面椭球高为 h、卯酉圈曲率半径为 N、WGS84 第一偏心率平方为 e²，取 u=N+h、v=N(1−e²)+h、k=u cos²φ+v sin²φ；地面椭球半轴满足 A²=uk、B²=vk。这些式子在赤道及极点均有有限值。对设备至目标的 ITRF 线段做椭球相交判断；不包含光行时、折射或地形起伏。该模型符合 [STK Line of Sight 的地面设施定义](https://help.agi.com/stk/LinkedDocuments/LineOfSight.pdf)，尚未做 STK 实机同输入对照。

地面站与卫星共用对象树、搜索、选择、定位、重命名、复制和删除。二维显示青绿色菱形及名称，三维显示青绿色菱形和独立选中环；地固视图中固定，惯性视图中随地球转动。只有地面站或没有对象的场景均可继续编辑。

三维地球仍使用参考椭球。标记以 `max(设备总椭球高, 100 m)` 留出显示间距，拾取与选中环使用相同位置；背面站点不可见、不可拾取。数值状态、属性预览、传感器指向和项目文件保留实际位置。地面站报告的 `station.height` 是地面椭球高，新增 `station.height_above_ground` 与 `station.observer_height` 分别给出离地高度和设备总椭球高；ITRF XYZ 对应设备位置。

## 场景、接口与文件

`ProjectGroundStation` 保存稳定 ID、显示名称、`GroundStationLocation`、最低仰角、`position_type` 和 `description`。ID 在所有对象中唯一；坐标类型与描述为元数据，不进入数值输入哈希。位置改变更新状态修订；名称、描述或坐标表示改变不会重算数值 Session。物理修改沿用后台候选准备与原子发布，失败或取消保留原场景；所有对象合计上限 10,000。

当前项目为 v51，必需 `GSTN` v3 段保存上述字段；只接受当前格式，旧项目明确报错，详见[项目文件](project_files.md)。复制和保存重开保留新增字段。Runtime 模型标识为 `ground-station-wgs84-fixed-itrf-v2`。

Python 示例：

```python
import orbelle as ob
from orbelle import units as u

project = ob.Project("观测任务", ob.Epoch(0), ob.Epoch(7_200_000_000_000))
station = project.objects.ground_station("楼顶天线",
    location=ob.Wgs84Location(longitude=u.deg(114.2), latitude=u.deg(22.3),
                             ellipsoid_height=u.m(350), height_above_ground=u.m(25)),
    minimum_elevation=u.deg(5),
    position_type=ob.GroundPositionType.GEODETIC,
    description="屋顶天线，设备离地 25 m")
xyz = station.location.coordinates(ob.GroundPositionType.CARTESIAN)  # 地面点，米
station.location = ob.Wgs84Location.from_coordinates(
    ob.GroundPositionType.CARTESIAN, xyz, height_above_ground=u.m(30))
station.description = "设备离地高度已调整"
project.save("station.orbelle")
```

C++ 使用 `orbelle::GroundPositionType`、`orbelle::ground_station_coordinates`、`orbelle::ground_station_location`，创建与修改字段位于 `orbelle::objects::GroundStation` / `GroundStationUpdate`；与 Python 共用校验、事务和计算实现。

当前不包含地形服务、方位仰角掩模、地形坡度、当地时间显示、通信/雷达模型或地面站子传感器。已有几何 Access 与卫星传感器能力见[可见性与传感器](access_sensors.md)。

## 验证

Core 测试使用 ERFA 已发布的 WGS84 参考坐标作正/逆变换对照，位置容限 1e−7 m；覆盖极点、日期变更线、高度边界、五种表示、非法输入，以及负地面高与塔顶视线的解析几何。文件独立样例手工编码 GSTN v3，由 C++ 和 Python 逐字节复现；同时验证损坏数据、元数据/物理输入身份、事务失败、复制与重开。Qt offscreen 验证分页、单位、无效输入、应用/重置/关闭与 320–920 逻辑像素布局。真实 GPU smoke 检查离地高度、显示间距和元数据应用不重算；执行方法见[构建说明](../development/build.md)。
