# 对象与 EOP 数据管理

第二步在项目持久化之上补齐对象管理和本地数据导入基础。桌面、Python 和 C++ 共用对象配置、数据导入和 Scenario 数据锁实现。数据入口现已合并到[统一数据中心](data_center.md)，支持 finals2000A 手动与自动下载。

## 对象操作

选择对象后，通过“对象”菜单或对象树右键执行：

| 操作 | 快捷键 | 行为 |
| --- | --- | --- |
| 重命名 | F2 | 改变显示名称；稳定 ID 和计算修订保持不变 |
| 复制对象 | Ctrl+D | 地面站复制名称与位置；卫星复制已应用的轨道、显示范围、模型类别、尺寸和路径；生成未使用的 ID；新 Session 从修订 1 开始 |
| 删除 | Delete | 确认后删除；选择相邻存续对象；删除最后一个对象后保留可继续插入的空场景 |

名称用于对象树、二维名称、时间轴和属性标题；数值结果与引用继续使用 ID。显示名称可以重复，ID 不可重复。复制后对副本的编辑不影响原对象。外部模型文件仍为共享引用，不复制磁盘资源。

复制、删除和数据更新后的重新计算暂停播放，按操作开始时刻准备完整候选场景，成功后整体发布。保留未编辑对象的 Session 修订、地图缩放和视图；失败或取消保留当前场景。状态栏提供“取消更新”。这些操作关闭已有属性页，未应用草稿不参与操作。当前没有撤销栈，删除前有确认提示。

项目保存显示名称和数据依赖清单；格式 v5 可读取 v1/v2/v3/v4，旧文件没有名称时默认为 ID。重新打开项目时重建 Session，不序列化运行中修订。

## 数据列表与获取

“数据 → 数据中心”左侧列出 finals2000A、C04、UTC / 闰秒，右侧显示所选数据的下载或导入时间、覆盖范围、有效截止时间、状态、来源与操作。内置数据标记“随程序附带”，旧目录没有获取记录时显示“未记录”。详情显示文件与依赖 SHA-256、完整覆盖范围和来源。

数据中心默认全量下载 finals2000A，首次安装后可自动更新，不需要选择日期或手工准备清单。右侧可导入完整 finals 原文件；选择 C04 后可导入已有 `manifest.json` 与同目录数据文件（支持 `iers-20-c04-0h-dxdy` / `iers-finals2000a`）。C04 原文件仍使用 `tools/data/import_c04.py` 转为快照。

Runtime 的 `DataCatalog` 调用 IERS Provider 校验清单、SHA-256、覆盖范围和数据行，复验临时副本后发布并记录到 SQLite。内容文件按原始字节哈希去重，快照目录按 Scenario 数据锁命名；相同导入幂等，已有版本不覆盖。底层旧 API `import_eop_snapshot()` 仍保留原来的清单哈希目录格式，便于兼容已有工具。

桌面使用 Qt `AppLocalDataLocation/data`，Windows 默认 `%LOCALAPPDATA%/OrbitForge/data`。旧 `eop` 目录在启动时按原身份导入并保留原文件；损坏条目的原因在窗口显示。测试将目录隔离到 smoke 输出，不改用户数据。导入后不依赖原文件位置，也不自动切换当前项目。

不提供历史版本列表、默认版本选择或“应用到当前场景”。打开或重新计算时检查覆盖分析时段的当前数据；有数据或实际使用的外推策略变化时须用户允许，随后一起重建所有对象的显示轨迹和太阳方向。拒绝则不打开或重算。一次计算始终使用同一组 Provider；失败或取消不会影响正在显示的结果。项目保存实际使用的数据哈希作为来源记录，旧记录的文件缺失不阻止用户确认后使用当前数据打开。

每类 EOP 可独立设置是否允许外推及 1–30 天上限，默认关闭。场景属性与计算来源记录实际外推；不改变原文件有效期，不延长 UTC / 闰秒有效期。覆盖仍不足时显示缺口与数据中心入口。

用户导入的数据没有嵌入项目文件，迁移到其他电脑时需要有覆盖分析时段的当前数据。此版本不提供缓存自动清理、UTC 闰秒数据独立更新或打开项目时自动下载缺失数据。

## Scenario 数据锁

每个计算中的 Scenario 持有不可变 `DataLock`，项目文件保存同一来源清单。当前包含 EOP 与 UTC 时间数据，记录快照 ID、文件 SHA-256、解释版本和配置 SHA-256，再计算整组依赖的总 SHA-256。改名不改变计算；重新计算采用不同数据时生成新 Scenario 修订，成功后才发布。场景属性可查看、复制“本次计算数据 SHA-256”。

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

catalog = of.DataCatalog(Path('data/shared-store'))
installed = catalog.install_eop('snapshot/manifest.json')
project.earth_orientation = catalog.load_eop(installed.lock_sha256).data_info
project.save('mission-updated.orbitforge')
```

Python/C++ 导入可指定存储目录；桌面自动发现的是其用户目录。绑定数据的完整场景覆盖在项目校验/保存时检查。无 ERFA 的构建仍支持项目和对象操作，EOP 导入明确报告功能不可用。
