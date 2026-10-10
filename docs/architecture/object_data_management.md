# 对象与 EOP 数据管理

第二步在项目持久化之上补齐对象管理和本地数据导入基础。桌面、Python 和 C++ 共用对象配置、数据导入和 Scenario 数据锁实现。当前导入界面是过渡入口；下一步按[统一数据中心方案](data_center.md)收敛管理和下载。

## 对象操作

选择对象后，通过“对象”菜单或对象树右键执行：

| 操作 | 快捷键 | 行为 |
| --- | --- | --- |
| 重命名 | F2 | 改变显示名称；稳定 ID 和计算修订保持不变 |
| 复制对象 | Ctrl+D | 复制已应用的轨道、显示范围、模型类别、尺寸和路径；生成未使用的 ID；新 Session 从修订 1 开始 |
| 删除 | Delete | 确认后删除；选择相邻存续对象；删除最后一个对象后保留可继续插入的空场景 |

名称用于对象树、二维名称、时间轴和属性标题；数值结果与引用继续使用 ID。显示名称可以重复，ID 不可重复。复制后对副本的编辑不影响原对象。外部模型文件仍为共享引用，不复制磁盘资源。

复制、删除和 EOP 切换暂停播放，按操作开始时刻准备完整候选场景，成功后整体发布。保留存续对象的 Session 修订、地图缩放和视图；失败或取消保留当前场景。状态栏提供“取消更新”。这些操作关闭已有属性页，未应用草稿不参与操作。当前没有撤销栈，删除前有确认提示。

项目保存显示名称和数据依赖清单；格式 v3 可读取 v1/v2，旧文件没有名称时默认为 ID。重新打开项目时重建 Session，不序列化运行中修订。

## 快照导入与选择

“场景 → 地球定向数据”列出内置和用户导入的快照，可查看 ID、SHA256、覆盖时间、来源、参考系、插值和时间数据版本。C04 标为事后解；Bulletin A 提示可能含预测值，原始 I/P 标记仍保存在 Provider 的逐时刻来源记录中。

导入入口接受已准备好的 `manifest.json` 和同目录数据文件，支持 `iers-20-c04-0h-dxdy` / `iers-finals2000a`。已有原始 IERS 文件先用 `tools/data/import_c04.py` 或 `tools/data/import_finals.py` 转为快照；各脚本的 `--help` 提供来源 URL、取回日期和截取日期参数。此步骤没有在线下载或自动更新。

Runtime 调用 IERS Provider 校验清单版本、字段、SHA256、覆盖范围和数据行，再把清单及数据复制到独占临时目录，复验后通过目录重命名发布。存储目录键为清单原始字节的 SHA256；相同导入幂等，已有版本不覆盖。并发相同导入会复验已有发布结果。失败清理本次临时文件，未承诺断电持久性。

桌面在 Qt `AppLocalDataLocation/eop` 保存快照；Windows 默认 `%LOCALAPPDATA%/OrbitForge/eop`。启动时重建目录列表并验证每个快照；损坏条目不加载，原因在数据窗口显示。测试将目录隔离到 smoke 输出，不改用户数据。导入后不依赖原始文件位置，也不自动切换当前项目。

没有场景时，“用于新建场景”设置本次运行的优先数据版本；若不覆盖新场景，会寻找其他覆盖版本。已有场景时，只有完整覆盖分析时段的条目能应用。成功切换后重建地固显示轨迹和太阳方向，并标记项目未保存；轨道的 GCRF 输入与计算 Session 保持不变。保存固定所选 EOP 和时间版本，重新打开要求精确匹配；缺失或损坏不能静默换为其他版本。

用户导入的数据没有嵌入项目文件，迁移到其他电脑时需另外导入相同快照。此版本不提供目录删除、UTC 闰秒数据更新或自动缺失数据下载。

## Scenario 数据锁

每个运行中的 Scenario 持有不可变 `DataLock`，项目文件保存同一清单。当前包含 EOP 与 UTC 时间数据，记录快照 ID、文件 SHA-256、解释版本和配置 SHA-256，再计算整组依赖的总 SHA-256。改名、复制和删除对象保持数据锁；显式切换数据生成新 Scenario 修订，成功后才发布。场景属性可查看、复制完整总哈希。

路径和来源 URL 不参与数据锁；移动本地文件或切换同内容镜像不改变依赖身份。数据内容、有效区间、参考系、插值或上游时间数据变化会改变依赖哈希。总哈希表示数据集合，不是包含全部轨道和显示输入的项目哈希。完整编码及迁移约定见[数据引用契约](scenario_data_lock.md)。

## 公共 API

`Project.renamed_satellite(id, name)`、`Project.with_satellite_copy(id, new_id, name)` 和 `Project.without_satellite(id)` 返回新值，失败不修改原值。`Scenario.without_object(id)` 返回新场景修订，存续 Session 和旧读者保持有效。

```python
from pathlib import Path
import orbitforge as of

project = of.load_project('mission.orbitforge')
object_id = project.satellites[0].orbit.object_id
project = project.renamed_satellite(object_id, '观测卫星')
project = project.with_satellite_copy(object_id, 'observer-copy', '观测卫星 副本')
project = project.without_satellite('observer-copy')

installed = of.import_eop_snapshot('snapshot/manifest.json', Path('data/eop-store'))
project.earth_orientation = of.load_eop_snapshot(installed).data_info
project.save('mission-updated.orbitforge')
```

Python/C++ 导入可指定存储目录；桌面自动发现的是其用户目录。绑定数据的完整场景覆盖在项目校验/保存时检查。无 ERFA 的构建仍支持项目和对象操作，EOP 导入明确报告功能不可用。
