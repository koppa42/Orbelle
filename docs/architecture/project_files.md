# OrbitForge 项目文件 v3

`.orbitforge` 是由 Application 模块读写的单文件、分段二进制容器。桌面、Python wheel 和 C++ SDK 使用相同实现。此版本保存可重建场景的输入，不保存 Provider 指针、GPU 资源或显示轨迹缓存。采用独立格式是为了精确保存纳秒整数和浮点输入，并允许以后为结果引用、其他对象和资源增加有版本的段。

## 当前工作流

桌面提供“打开”“保存”“另存为”，快捷键为 Ctrl+O、Ctrl+S、Ctrl+Shift+S。插入对象改用 Ctrl+Shift+I。启动页也可打开项目。保存内容是**已应用**的场景和属性；属性页中尚未应用的草稿不自动提交。场景创建、轨道、对象外观和对象轨迹范围的应用都会标记未保存；新建、打开和退出前可保存、放弃或取消。

保存包含场景名称、起止时刻、保存时的当前时刻，以及每个卫星的 ID、二体/J2 传播器、初始状态、输入参考系、显示步长、二维/三维轨迹和名称选项、轨迹范围、模型类别、尺寸及外部路径。当前时刻写入文件，但播放和浏览时间本身不反复标记项目修改。相机、分屏布局、全局视图开关和未应用草稿属于本轮尚未保存的工作区状态。

打开时先校验文件，在后台重建全部 Session、轨迹和模型，再编译图形资源并整体发布；状态栏可取消打开。数据版本缺失、计算或模型读取失败，以及取消准备时保留当前项目。载入的计算快照从新修订开始，稳定对象 ID 和计算输入保持不变；运行中的任务和修订计数不序列化。

EOP 引用包含快照 ID、SHA256、参考系、插值版本、覆盖区间，以及其依赖的时间数据 ID、SHA256 和 UTC 有效期。桌面要求与已安装的快照精确匹配，不自动换成“最新”数据。来自 Python 的未绑定项目可在首次打开时选择覆盖其时段的已安装快照；此绑定会标记为未保存，保存后固定该版本。通过“场景 → 地球定向数据”导入和选择快照，详见[对象与数据管理](object_data_management.md)。

v2 增加独立的对象显示名称，重命名不改变稳定 ID、计算输入或 Session 修订。v3 增加必需的 LOCK 段，保存 Scenario 的依赖清单与总 SHA-256，覆盖 EOP、其依赖的时间数据和解释配置。读取兼容 v1/v2；旧文件没有显示名称时显示其 ID，数据锁从原 DATA 信息推导。保存统一写为 v3，旧版程序明确拒绝未知版本。运行中的删除、复制和 EOP 切换保留存续对象的 Session 修订；从文件重新打开仍从新修订开始。

外部三维模型按项目所在目录保存相对路径，跨盘时保留绝对路径；另存为会重新计算路径。模型及其纹理、外部缓冲文件仍需随项目另行保留，当前容器不嵌入资源，也不提供外部模型内容版本校验。模型缺失时拒绝打开，不静默改成点标记。内置示意模型无此文件依赖。

## 二进制约定

所有整数按小端编码，`f64` 是 IEEE-754 binary64 原始位。`epoch` 是自 2000-01-01 12:00:00 TAI 起的有符号 64 位纳秒，以二进制补码保存。字符串为 `u32` 字节长度后跟 UTF-8，无结尾 NUL；拒绝非法 UTF-8 和内嵌 NUL。长度和浮点输入没有文本舍入。

文件头共 24 字节：

| 字段 | 类型 | v3 值或含义 |
| --- | --- | --- |
| magic | 8 bytes | `ORBFPRJ` 后跟 LF，即 `4f 52 42 46 50 52 4a 0a` |
| format_version | u32 | 3（读取也接受 1、2） |
| payload_length | u64 | 后续全部段的总字节数 |
| payload_crc32 | u32 | 后续全部段的 CRC-32/ISO-HDLC |

每个段由 `tag:u32, version:u32, length:u64, crc32:u32, body:length bytes` 组成；校验覆盖 body。tag 的四个小端字节直接组成 ASCII 名称。v1 段版本均为 1；v2/v3 的 OBJS 段版本为 2，SCNE / DATA / LOCK 为 1。SCNE 和 OBJS 必须各出现一次，DATA 可选且最多一次，v3 中 LOCK 必须出现一次。顺序不影响读取，写入顺序固定为 SCNE、OBJS、可选 DATA、LOCK。未知文件版本、段版本、未知段、重复段和多余字节均拒绝，避免旧程序丢弃新字段后重新保存。

| 段 | body 字段顺序 |
| --- | --- |
| SCNE | 名称 string；first、last、current 各 epoch；中心天体模型 ID string |
| OBJS | 对象数 u32；随后依次为下述卫星记录 |
| DATA | EOP ID、SHA256、来源 URL、参考系、插值版本各 string；覆盖 first、last 各 epoch；时间数据 ID、SHA256、来源 URL 各 string；UTC 有效 MJD 起点、终点各 u32 |
| LOCK | [数据锁规范编码](scenario_data_lock.md)；随后是清单总 SHA-256 string |

卫星记录字段依次为：ID string；显示名称 string（v2/v3，空字符串表示使用 ID）；传播器 u32（1 二体，2 标准一阶 J2 长期项）；编辑参考系 u32（1 GCRF，2 ITRF）；初始历元 epoch；三分量位置和三分量速度各 f64（m、m/s）；显示步长 u32（s）；轨迹范围 u32（0 一圈、1 场景、2 自定义）；前后时长各 f64（min）；二维轨迹、二维名称、三维轨迹各 u8 boolean；模型类别 u32（0 点、1 内置、2 外部）；模型尺寸 f64（m）；模型路径 string。

v1–v3 中所有持久化初态均为地心 GCRF，中心天体常数固定为 `earth-iers2003-v1`。二体初态为瞬时 Cartesian 状态，J2 初态为平均椭圆的辅助 Cartesian 状态；J2 重建时同时启用全部三个长期项。编辑参考系仅用于恢复输入控件的表示，不能把持久化 GCRF 数值解释为 ITRF。模型定义发生不兼容变化时必须变更文件/段版本，或引入明确迁移。

读取上限为 64 MiB、10000 个卫星、每个字符串 16384 字节、64 个数据引用。检查长度后才分配和读取；拒绝非有限数、重复 ID、不一致的场景区间、非法显示选项及不覆盖场景的数据引用。v3 还检查 LOCK 总哈希及其与 DATA 的一致性；即使重新计算了 CRC，缺失或不一致的 LOCK 仍拒绝。轨道物理有效性由 Runtime 在重建 Session 时继续验证。CRC 和 SHA-256 用于完整性与版本核验，不是来源签名或安全认证。

## 保存和恢复

先在内存中完成校验与编码，再在目标目录下创建独占临时目录，写入并刷新临时文件，最后原子发布到目标路径。Windows 使用 MoveFileExW，POSIX 使用 rename（覆盖）或 link（不覆盖）。失败会清理本次临时文件；发布前的失败保留已有目标。此处承诺原子替换，不声称对所有网络文件系统或断电场景具备事务持久性保证。

C++：`application::Project`、`save_project/load_project`、`runtime::make_project_scenario`。Python：`Project`、`OrbitInput`、`OrbitDisplay`、`ProjectSatellite`、`load_project`。Python 保存默认拒绝覆盖，传入 `overwrite=True` 才替换已有文件。`Project.satellites` 使用列表值转换，修改已有对象后应把列表赋回；`add_satellite()` 自动继承项目时段并拒绝重复 ID。

```python
from pathlib import Path
import orbitforge as of

project = of.Project("Mission", of.Epoch(0), of.Epoch(7_200_000_000_000))
orbit = of.OrbitInput()
orbit.object_id = "Satellite1"
project.add_satellite(orbit)
project.save(Path("mission.orbitforge"))

restored = of.load_project("mission.orbitforge")
states = restored.to_scenario().sample([restored.first, restored.last])
```

上述历史时段与桌面附带的 2000 年示例 EOP 匹配。完整双传播器示例见 `examples/python/save_project.py`。无界面传播只使用轨道输入，不隐式加载 EOP 或外部模型；桌面打开才验证这些显示依赖。
