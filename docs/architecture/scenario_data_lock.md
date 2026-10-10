# Scenario 数据引用与哈希

每个 Scenario 持有不可变 `DataLock`。项目保存数据引用，统一数据中心负责实际文件；文件可以移动，场景始终绑定确定的数据内容与解释配置。当前实现覆盖 EOP 和它依赖的 UTC/TAI 时间数据；SPICE、星历目录和外部模型尚未接入此锁。

## 引用字段

| 字段 | 含义 |
| --- | --- |
| `role` | 依赖用途；当前为 `earth-orientation`、`utc-time`，同一锁内不能重复 |
| `snapshot_id` | 固定快照 ID，不能写 `latest` 之类的浮动别名 |
| `content_sha256` | Provider 实际读取的数据文件原始字节 SHA-256，64 位小写十六进制 |
| `interpretation_version` | 数据读取与解释规则版本 |
| `configuration_sha256` | 有效期、坐标系、插值及上游依赖等解释配置的 SHA-256 |

`DataLock.sha256` 对整个有序引用清单计算 SHA-256。它是数据依赖集合的身份，不是整个项目或计算结果的哈希；重命名对象、修改轨道或播放时刻均不改变数据锁。未来结果缓存仍需另外包含计算输入、模型和算法版本。

本地路径、下载时间和来源 URL 不参与锁，来源 URL 仍保存在项目 DATA 中用于溯源。同一快照的同内容镜像不会改变锁；快照 ID 本身属于身份，另起 ID 会产生不同的锁，即使文件内容相同。数据中心可复用同一内容文件，但不能只依据文件哈希把不同解释配置合并。

## 规范编码 v1

编码不使用 JSON。字符串采用 `u32` 小端字节长度加字符串原始字节，不包含 NUL 或结尾换行；项目中的字符串必须为合法 UTF-8。所有 SHA-256 均以小写十六进制字符串写入。引用按 ASCII `role` 升序排列，最多 64 项。

清单字节顺序：

1. 字符串 `orbitforge-data-lock-v1`。
2. 引用数量 `u32`。
3. 每个引用的五个字符串，顺序与上表相同。

对这些字节计算总 SHA-256；总哈希不包含自身。`canonical_bytes()` 暴露同一编码，供其他实现独立验证。空清单明确表示尚未绑定数据，其 SHA-256 为 `81700a60bcada79003d1ea38a39cbea749f3997c1cdea22737be0bee6b7aeab1`。

`utc-time` 使用解释版本 `orbitforge-utc-tai-v1`。其配置依次编码该版本字符串、UTC 有效 MJD 起点 `u32`、终点 `u32`，再取 SHA-256；文件内容哈希对应时间数据原文。

`earth-orientation` 使用解释版本 `orbitforge-eop-reader-v1`。其配置依次编码该版本字符串、参考系字符串、插值版本字符串、覆盖起止 TAI 纳秒各 `i64`（补码小端）、**仅含上述时间引用的 DataLock 总哈希字符串**，再取 SHA-256。因此时间内容、版本 ID 或有效期改变，即使 EOP 文件不变，也会改变 EOP 配置及整个数据锁。EOP 内容哈希对应实际读取的快照文件；如果该文件是源文件的截取子集，不能拿完整下载源文件的哈希替代。

解释或规范编码发生不兼容变化时必须使用新版本并提供明确迁移，不能修改旧版本含义。SHA-256 核验内容及身份一致性，不认证数据发布者。

## 场景和项目行为

| 操作 | 数据锁行为 |
| --- | --- |
| 新建纯 GCRF 无界面 Scenario / 未绑定 Project | 空锁；不自动访问数据中心或网络 |
| 桌面新建场景 | 绑定实际选择的 EOP 和时间数据 |
| 修改、复制、删除对象 | 保留原锁；对象计算修订按既有规则处理 |
| `Scenario.with_data_lock(lock)` | 相同锁不增修订；不同锁返回新 Scenario 修订，旧读者及 Session 保持有效 |
| 桌面显式切换数据 | 准备并验证候选场景，成功后一起发布 Provider 和新锁；取消或失败保留原场景 |
| 保存 | 核对运行中锁与当前 Provider 信息一致；写入项目 v3 的 LOCK 段 |
| 打开 v3 | 校验清单 SHA-256 和 DATA/LOCK 一致性；桌面还要求安装了精确匹配的数据 |
| 打开 v1/v2 | 从原 DATA 推导锁；原本未绑定的项目保持未绑定，桌面首次选择数据后标记修改；再次保存写 v3 |

v3 中，即使没有任何依赖，LOCK 也必须存在。缺失 LOCK、根哈希错误、DATA 与 LOCK 不一致均拒绝，不能借此退化为自动选择最新数据。已绑定项目缺少数据时不会静默改绑。

`DataLock` 是引用值，不自行加载文件或下载数据。无界面 `Project.to_scenario()` 保存这份身份信息，当前二体/J2 的 GCRF 传播不需要 EOP；调用者真正使用外部数据时仍应由 resolver 校验并加载相应 Provider。当前桌面已经通过 EOP 目录执行精确匹配，通用 resolver 属于[数据中心后续工作](data_center.md)。

## API

C++ 提供 `DataReference`、`DataLock`、`eop_data_lock`、`Project::data_lock()` 和 `Scenario::with_data_lock()`。Python 的引用和锁属性只读，`references` 返回值副本：

```python
import hashlib
import orbitforge as of

project = of.load_project('mission.orbitforge')
lock = project.data_lock
assert hashlib.sha256(lock.canonical_bytes()).hexdigest() == lock.sha256
assert project.to_scenario().data_lock.sha256 == lock.sha256
for ref in lock.references:
    print(ref.role, ref.snapshot_id, ref.content_sha256)
```

场景属性显示完整的数据依赖 SHA-256，可以复制用于对照。当前时间 Provider 仍由构建时的只读表提供；动态时间版本加载、其他数据类型和文件打包均另行实现。
