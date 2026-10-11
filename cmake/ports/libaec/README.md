# libaec 下载来源

沿用 vcpkg 基线 `9b965a116838c6cdcd36bca60d1b81b030c8ab8d` 的 libaec 1.1.6 构建配置，将返回 HTTP 429 的 DKRZ GitLab 归档替换为[同版本的官方 GitHub 发布包](https://github.com/Deutsches-Klimarechenzentrum/libaec/releases/tag/v1.1.6)。其他依赖继续使用固定基线。

发布包固定 SHA-512；其 SHA-256 为 `a469be4d835127e358c4f97de74943a54fbcb870aaf03cd2303c1dcc9fd4af4b`，与 GitHub 发布资产元数据一致。与基线原始归档对照，共有文件内容一致；发布包附带生成的 Autotools 文件，未包含部分仓库辅助文件。
