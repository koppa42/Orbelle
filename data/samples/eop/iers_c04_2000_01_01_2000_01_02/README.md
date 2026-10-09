# 圆轨道图形示例的 EOP 快照

来自 IERS 20 C04（0h dX/dY、ITRF2020）文件的 2000-01-01、2000-01-02 两个 UTC 午夜节点。用于 P0 圆轨道示例的 Earth/ITRF 显示转换，不扩展其分析能力或公开数据有效期。

原文来源、获取日期、原文及本快照 SHA256 均记录在 `manifest.json`。导入使用仓库 `tools/data/import_c04.py`，运行时由 IERS Provider 验证散列和覆盖范围。

OrbitForge 零历元为 2000-01-01 12:00:00 TAI，对应 2000-01-01 11:59:28 UTC。本快照覆盖零历元至 +300 s 的全部 6 个标准采样点。插值方法沿用 `c04-hermite-tai-v1`，不外推、不隐式访问网络。
