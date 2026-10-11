# CI 与版本发布

GitHub Actions 在每个 Pull Request、`main` 更新及手动运行时执行 CI。推送 `vX.Y.Z` 标签会在标签指向的提交上重新运行同一套检查；全部成功后，自动创建 GitHub Release。

## 检查范围

| 检查 | 内容 |
| --- | --- |
| Versions and tooling | CMake、Python、vcpkg 版本一致性；数据导入、交付与发布脚本测试 |
| Linux C++, sanitizers and wheel | Ubuntu 24.04：完整无界面 C++ 测试、ASan/UBSan、SDK 安装、Python wheel 构建 |
| Linux SDK and installed Python API | 新 runner，不检出源码；校验交付散列、迁移目录、构建 SDK 消费端、离线安装 wheel，运行 Python API、示例和 C++/Python 对照 |
| Windows desktop, SDK and wheel | Windows Server 2025 / VS 2022：完整桌面构建、CTest 与 Qt offscreen 测试、便携包、SDK 与 wheel 构建，解压 SDK 后编译消费端，在新虚拟环境中运行 Python API 测试 |
| CI passed | 汇总门槛；任何检查失败、跳过或取消都会失败 |

建议在 GitHub 的 `main` 分支规则中将 **CI passed** 设为必需检查。工作流本身不会修改仓库的分支保护设置。

依赖使用仓库固定的 vcpkg 基线与 overlay，并缓存编译结果。首次 Windows 构建需要从源码编译 Qt、VSG、Rocky、GDAL 等依赖，耗时会明显长于后续运行。失败诊断保留 14 天，测试交付物和发布候选包保留 7 天。

普通托管 runner 不承担真实 Vulkan GPU 验收；offscreen 测试通过不代表三维呈现或性能达标。带 GPU 的验证仍按[构建说明](build.md)执行。联网 IERS 服务检查是可选测试，CI 不依赖实时数据服务。

## 发布内容

| 文件 | 用途 |
| --- | --- |
| `OrbitForge-X.Y.Z-win-x64.zip` | Windows x64 便携桌面，解压后运行 `OrbitForge.exe` |
| `OrbitForge-X.Y.Z-sdk-win-x64.zip` | Windows x64 / VS 2022 Release C++ 无界面 SDK |
| `OrbitForge-X.Y.Z-sdk-linux-x64.tar.gz` | Ubuntu 24.04 x86_64 Release C++ 无界面 SDK |
| `orbitforge-X.Y.Z-cp313-cp313-win_amd64.whl` | Windows x64 / CPython 3.13 |
| `orbitforge-X.Y.Z-cp313-cp313-linux_x86_64.whl` | Ubuntu 24.04 x86_64 / CPython 3.13 |
| `SHA256SUMS` | 上述五个文件的 SHA-256 |

SDK 包含计算库、头文件、CMake 配置、样例数据及链接依赖。将 SDK 根目录和其 `dependencies` 子目录一起加入 `CMAKE_PREFIX_PATH`；Windows 消费程序运行时还需将 `dependencies/bin` 加入进程 `PATH`。具体用法见包内 `README.txt`。

Python wheel 在匹配平台安装：`python -m pip install <下载的.whl文件>`。源码支持 Python 3.11+，首版自动发布只提供 CPython 3.13 二进制。Linux wheel 是本机构建包，不声称 manylinux 兼容；其他发行版或 Python 版本请从源码构建。工作流上传到 GitHub Releases，不发布到 PyPI。

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

发布作业仅使用本次标签构建产生的文件，核对五种产物和 wheel 版本后计算校验值，先创建草稿，上传并核对全部附件，最后公开。说明包含平台要求与 GitHub 自动生成的变更记录。上传失败会留下未公开的草稿，可在 Actions 中重新运行失败作业；已经公开的同名版本不会被覆盖，应修复后发布新的版本号。

工作流只需 GitHub 自动提供的 `GITHUB_TOKEN`，不需要额外 PAT。测试作业只有读取权限，只有发布作业具有 `contents: write`；Actions 固定到完整提交 SHA，Dependabot 每月检查更新。
