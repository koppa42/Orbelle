# CI 与版本发布

所有改动先推送主题分支，再通过 Pull Request 进入 `main`；禁止直接推送 `main`。合并前更新到最新目标分支，确认当前提交的 **CI passed** 成功，再由维护者确认合并。提交或推送分支不代表授权合并。

## 触发与分层

| 触发 | 执行范围 |
| --- | --- |
| 代码、测试、CI、依赖或资源 PR | 完整 Windows / Linux 构建、测试、交付验证与 Python 版本矩阵 |
| 纯文档 PR | 版本一致性及工具测试；跳过原生构建和 Python wheel 矩阵 |
| 合并后 `main` 更新 | 版本一致性及工具测试；检查两平台共享依赖缓存，缺失时只编译依赖，不重复构建和测试产品 |
| 手动运行 | 完整检查，可用于复核当前 `main` 或排查问题 |
| `vX.Y.Z` 标签 | 在标签提交上重新运行完整检查；全部成功后创建 GitHub Release |

纯文档的范围限于根目录 `README.md`、`AGENTS.md` 和 `docs/` 下的 Markdown 文件。其他路径、空差异或无法确定的差异均执行全量检查。分类对比 PR 模拟合并提交与其目标分支父提交，包含 PR 的全部提交以及重命名前后的路径；不能仅凭最后一次提交或文件扩展名跳过测试。

普通 PR 只做验证，不创建 Release，也不上传 `*-release-assets`。为测试解压后的 SDK、已安装 wheel 以及跨平台 Python 矩阵，CI 仍会构建测试用安装包，并在作业之间传递 `linux-delivery` 和 `windows-python-dependencies`；这些是测试输入。诊断日志另行保留。发布附件的整理与上传仅在 `release-tag` 非空的标签流程中执行，最终公开 Release 仍须等待全部检查成功。

PR 使用 GitHub 默认的[模拟合并引用](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#pull_request)运行。合并后不再完整重跑的前提是合并前已基于最新 `main` 验证；目标分支变化时先更新 PR 并等待新的检查结果。若启用合并队列，需先接入 `merge_group` 事件。

## 检查范围

| 检查 | 内容 |
| --- | --- |
| Versions and tooling | CMake、Python、vcpkg 版本一致性；数据导入、交付与发布脚本测试 |
| Linux C++, sanitizers and Python 3.13 | Ubuntu 24.04：完整无界面 C++ 测试、ASan/UBSan、SDK 安装、基准 Python wheel 构建 |
| Linux SDK and installed Python API | 新 runner，不检出源码；校验交付散列、迁移目录、构建 SDK 消费端、离线安装 wheel，运行 Python API、示例和 C++/Python 对照 |
| Windows desktop, SDK and Python 3.13 | Windows Server 2022 / VS 2022：完整桌面构建、CTest 与 Qt offscreen 测试、便携包、SDK 与基准 wheel 构建，解压 SDK 后编译消费端，在新虚拟环境中运行 Python API 测试 |
| Python 3.11 / 3.12 / 3.14 / 3.15 × Windows / Linux | 复用相应平台的原生依赖，分别构建 wheel，在新虚拟环境离线安装后运行 API 测试及五个示例；任一版本失败都阻止发布 |
| CI passed | 按本次范围检查全部预期作业；应执行的作业失败、取消或意外跳过都会失败，仅允许纯文档和缓存维护模式中明确计划的跳过 |

GitHub 的 `main` 分支规则应要求通过 PR 合并、**CI passed** 成功，并要求分支合并前保持最新；禁用绕过和强制推送。仓库中的规范和工作流不会自动修改 GitHub 分支保护设置。

## 缓存与耗时

依赖使用仓库固定的 vcpkg 基线与 overlay，并缓存编译结果。Windows CI 使用专用的 `x64-windows-release` triplet，固定 v143 工具链，只构建实际用于测试和发布的 Release 依赖。本地默认 triplet 不变。首次 Windows 构建需要从源码编译 Qt、VSG、Rocky、GDAL 等依赖，耗时会明显长于后续运行；即使后续依赖失败，已完成的包也会保存到部分缓存，供下次继续使用。缓存区分 runner 镜像和依赖配置，vcpkg 另按编译器及包 ABI 校验是否可复用。失败诊断保留 14 天，测试交付物和发布候选包保留 7 天。

GitHub 将 PR 生成的缓存限制在该 PR 的合并引用中，其他 PR 和 `main` 不能读取；PR 可以读取目标分支的缓存，详见[缓存访问范围](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache)。因此保留 `main` 上的依赖缓存维护：精确命中时不运行依赖安装，未命中时生成供后续 PR 使用的共享缓存。首次建立缓存、依赖变化或 runner 工具链变化仍可能需要较长编译，不能承诺每次都命中缓存。

排查耗时时先区分 `Install dependencies` / `Warm shared dependencies` 与项目构建、测试、打包。查看缓存是否命中、vcpkg 恢复了多少包、哪些包重新编译；不要用减少 Python 支持版本或跳过行为测试来掩盖依赖冷启动问题。

本地日常开发只需验证 Python 3.13；多版本兼容性由 CI 负责，不必在开发机安装多套 Python。`pyproject.toml` 的 `[tool.orbitforge.ci]` 统一定义基准版本和测试版本，CI 矩阵与发布附件检查共用这份配置。桌面和 SDK 每个平台只构建一次；其他解释器版本只构建 Python 包。

普通托管 runner 不承担真实 Vulkan GPU 验收；offscreen 测试通过不代表三维呈现或性能达标。带 GPU 的验证仍按[构建说明](build.md)执行。联网 IERS 服务检查是可选测试，CI 不依赖实时数据服务。

## 发布内容

| 文件 | 用途 |
| --- | --- |
| `OrbitForge-X.Y.Z-win-x64.zip` | Windows x64 便携桌面，解压后运行 `OrbitForge.exe` |
| `OrbitForge-X.Y.Z-sdk-win-x64.zip` | Windows x64 / VS 2022 Release C++ 无界面 SDK |
| `OrbitForge-X.Y.Z-sdk-linux-x64.tar.gz` | Ubuntu 24.04 x86_64 Release C++ 无界面 SDK |
| `orbitforge-X.Y.Z-cpNNN-cpNNN-win_amd64.whl` | Windows x64；CPython 3.11～3.15，每个版本一个 wheel |
| `orbitforge-X.Y.Z-cpNNN-cpNNN-linux_x86_64.whl` | Ubuntu 24.04 x86_64；CPython 3.11～3.15，每个版本一个 wheel |
| `SHA256SUMS` | 上述十三个文件的 SHA-256 |

SDK 包含计算库、头文件、CMake 配置、样例数据及链接依赖。将 SDK 根目录和其 `dependencies` 子目录一起加入 `CMAKE_PREFIX_PATH`；Windows 消费程序运行时还需将 `dependencies/bin` 加入进程 `PATH`。具体用法见包内 `README.txt`。

Python wheel 在匹配平台安装：`python -m pip install <下载的.whl文件>`。文件名中的 `cp311`～`cp315` 分别对应 CPython 3.11～3.15，采用普通 GIL 构建；暂不发布 PyPy 或 free-threaded Python 变体。Linux wheel 是本机构建包，不声称 manylinux 兼容；其他发行版或 Python 版本请从源码构建。工作流上传到 GitHub Releases，不发布到 PyPI。

## 发布步骤

以发布 `0.1.1` 为例：

```powershell
python tools/ci/version.py --set 0.1.1
git add CMakeLists.txt pyproject.toml vcpkg.json
git commit -m "chore: prepare v0.1.1"
# 按正常流程提交 PR、等待 CI 并合入 main。
git switch main
git pull --ff-only
python tools/ci/version.py --tag v0.1.1
git tag -a v0.1.1 -m "OrbitForge v0.1.1"
git push origin v0.1.1
```

触发条件是推送 **Git 标签**，提交信息写 `v0.1.1` 不会发布。三个清单的版本必须相同，并与标签精确匹配；`v1.2`、`v01.2.3`、`v1.2.3-rc1` 等格式会在构建前被拒绝。当前流程只处理稳定的三段版本号。

发布作业仅使用本次标签构建产生的文件，核对桌面、两套 SDK、完整 Python 矩阵的 wheel 及其版本后计算校验值，先创建草稿，上传并核对全部附件，最后公开。说明包含平台要求与 GitHub 自动生成的变更记录。上传失败会留下未公开的草稿，可在 Actions 中重新运行失败作业；已经公开的同名版本不会被覆盖，应修复后发布新的版本号。

工作流只需 GitHub 自动提供的 `GITHUB_TOKEN`，不需要额外 PAT。测试作业只有读取权限，只有发布作业具有 `contents: write`；Actions 固定到完整提交 SHA，Dependabot 每月检查更新。
