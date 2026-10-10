# 外部数据清单

这里保存外部数据快照的版本清单；大型数据文件不进入仓库。固定圆轨道案例无需外部数据；可选 ERFA 时间 Provider 使用 `iers_leap_seconds_2026_07.json` 指定的 IERS Bulletin C 72 原文，小型原文保存在 `data/samples/time/`。

该清单记录原始数据来源、获取日期、SHA256、UTC/MJD 有效区间和失效策略。构建时校验原文并编译为只读表，打包时保留原文与来源；运行时不自动下载或替换。此数据格式只服务于首个时间 Provider，尚未形成所有外部数据通用的 Schema 或 Application 数据目录。

EOP 已有独立 schema 1 清单，与数据一同保存在 `data/samples/eop/<快照目录>/`。Runtime 显式读取该目录中的 `manifest.json`，不要求以本目录作为全局注册表。格式、插值和覆盖约定见[时间与参考系契约](../../docs/architecture/time_frames.md)。`tools/data/import_c04.py` 可从本地 IERS C04 原文件创建新的不可覆盖快照。

SPICE 也已有独立 schema 1 清单，放在 `data/samples/spice/<快照目录>/`，记录有序内核、原始/派生文件散列、来源、生产者、模型和允许的 TAI 窗口。Runtime 通过显式清单与工作目录加载，生成 Provider 拥有的运行副本；格式与限制见 [CSPICE 接口](../../docs/architecture/spice.md)。样例目录内的 README 说明派生过程和分发条件。

项目通过 [Scenario 数据锁](../../docs/architecture/scenario_data_lock.md)固定 EOP 和时间依赖；桌面数据中心已有统一目录、EOP 导入及 finals2000A 自动/手动下载。动态时间数据加载、其他数据类型及缺失旧版本获取按[数据中心方案](../../docs/architecture/data_center.md)继续推进。开发机下载目录可配置为 `local/data`；正式结果不放在这里。禁止只修改清单中的有效期来延长过期数据的适用范围。

