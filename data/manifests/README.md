# 外部数据清单

本目录保存随程序分发的数据清单；大型在线数据保存在用户的数据中心目录。固定圆轨道案例无需外部数据，其他计算按所用 Provider 显式检查数据与覆盖范围。

`iers_leap_seconds_2026_07.json` 指定 IERS Bulletin C 72 闰秒原文，文件位于 `data/samples/time/`。清单记录来源、获取日期、SHA-256、UTC/MJD 有效区间和失效策略。构建时核验原文并生成内置只读表，打包时保留原文与来源。运行时也可独立加载 `Leap_Second.dat`；数据中心更新当前 UTC 文件不会修改已创建的 Provider。

EOP 使用 schema 1 清单，与数据一起保存在 `data/samples/eop/<快照目录>/`。Runtime 显式读取 `manifest.json`，支持 IERS C04 和 finals2000A；本目录不是运行时的全局注册表。格式、插值和覆盖约定见[时间与参考系契约](../../docs/architecture/time_frames.md)。`tools/data/import_c04.py` 可从本地 C04 原文件截取不可覆盖的测试快照；桌面数据中心可直接导入原文或下载完整文件。

SPICE 使用独立的 schema 1 清单，位于 `data/samples/spice/<快照目录>/`，记录有序内核、文件散列、来源、模型及允许的 TAI 窗口。Runtime 通过显式清单与工作目录加载，并创建 Provider 拥有的运行副本；格式与限制见 [CSPICE 接口](../../docs/architecture/spice.md)。样例目录保留派生过程及第三方分发条件。

[数据中心](../../docs/architecture/data_center.md)使用 SQLite 管理当前数据，C04、finals2000A 和 UTC/闰秒均支持完整下载、独立自动更新及本地导入。项目通过 [Scenario 数据引用](../../docs/architecture/scenario_data_lock.md)记录实际使用的内容与配置哈希；打开或重新计算时发现变化须经用户确认，普通场景不要求保留旧快照。

仓库中的 EOP/SPICE 是有限时段的测试与示例数据，不代替当前在线数据。运行输出写入 `out/`，本机下载可放在 `local/data/`。不能只修改清单的有效期来延长数据覆盖；有限 EOP 外推使用独立、显式的策略，UTC 有效期不外推。
