# CI 与版本发布

所有改动先推送主题分支，再通过 Pull Request 进入 `main`；禁止直接推送 `main`。合并前更新到最新目标分支，确认当前提交的 **CI passed** 成功，再由维护者确认合并。提交或推送分支不代表授权合并。

## 触发与测试范围

Windows 桌面发布同时生成完整便携包和 `app`、`runtime`、`resources` 三个更新分包及更新清单，全部纳入 `SHA256SUMS`。发布前核对整包与分包内容一致、版本与路径合法、文件和归档散列正确。资源采用运行白名单，固定测试输入留在验证目录；详见[桌面软件更新](../architecture/application_updates.md)。

| 触发 | 执行范围 |
| --- | --- |
| 代码、测试、CI、依赖或资源 PR | Windows / Linux 完整数值与行为回归、Python 3.11–3.15 API 测试；不构建发布包 |
| 纯文档 PR / 纯文档合并到 `main` | 工作流静态检查、版本一致性及工具测试；跳过原生构建和 Python 矩阵 |
| 其他 `main` 更新 | 两平台完整 C++ / 桌面回归和基准 Python 3.13 测试；不重复 sanitizer 和其他 Python 版本，不构建发布包 |
| 手动运行 `CI` | 与代码 PR 相同的完整常规测试；不构建发布包 |
| 手动运行 `Sanitizers` | 独立执行全量 ASan/UBSan；不重复原生 Release 或 Python 矩阵 |
| `vX.Y.Z` 标签 | 完整数值与行为回归、发布包构建、安装与迁移验证；全部成功后创建 GitHub Release |

ASan/UBSan 仅在 **Actions → Sanitizers → Run workflow** 中手动执行；PR、`main` 和发布标签均不自动触发，也不配置定时任务。手动运行直接检查全量用例，无分层选项或排除清单。修改内存管理、文件解析、原生接口、重大重构或发布前，按风险手动补充检查。同一 PR 的新提交继续取消过期 CI 运行。

纯文档的范围限于根目录 `README.md`、`AGENTS.md` 和 `docs/` 下的 Markdown 文件。PR 分类对比模拟合并提交与其目标分支父提交；`main` 分类对比 push 事件的 `before..after`，校验当前检出等于 `after` 且推送为快进。两者均包含整个变更范围及重命名前后的路径，不能仅凭最后一次提交或文件扩展名跳过测试。其他路径、空差异、历史缺失或无法确定的差异，PR 保留完整测试，`main` 保留基准测试。

测试需要先编译 C++ 和 Python 原生扩展，但不需要创建可分发的安装包。普通 PR 和 `main` 不生成桌面 ZIP、SDK 发布归档或 wheel，不创建 Release，也不上传 `*-release-assets`。Python 测试按 `pyproject.toml` 的相同功能配置直接使用 CMake 编译扩展，将扩展及配套 Python 文件放入独立虚拟环境，再运行 API 测试与示例。wheel 元数据、打包布局和安装行为另由标签流程验证。

PR、`main` 与发布安装验证通过 `tools/ci/validation_inputs.py` 整理相同的独立测试目录，包含 Python 测试、十二个示例、SDK 消费端、固定参考值、项目文件样例及测试使用的 EOP / 闰秒 / SPICE 片段数据。测试目录保留相对路径，但不复制 Python 包源码或 C++ 实现；计算能力必须来自被测安装。普通 Python CI 也从此目录运行，提前发现发布验证输入遗漏。

`linux-python-dependencies` 和 `windows-python-dependencies` 由各平台独立的轻量依赖作业生成，供本次 Python 矩阵复用已编译的无界面依赖，内容不包括 Orbelle 安装包。诊断日志另行保留。标签流程还传递 `linux-delivery`，供没有源码检出的新 runner 验证 SDK 和 wheel，然后整理发布附件。

PR 使用 GitHub 默认的[模拟合并引用](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#pull_request)运行。合并后在实际 `main` 提交上执行基准测试，同时更新可供后续 PR 使用的依赖缓存。多版本兼容性在合并前完成；目标分支变化时先更新 PR 并等待新的检查结果。若启用合并队列，需先接入 `merge_group` 事件。

## 检查范围

| 检查 | 内容 |
| --- | --- |
| Versions and tooling | actionlint 工作流静态检查；CMake、Python、vcpkg 版本一致性；数据导入、CI 规划、交付与发布脚本测试；标签提交归属检查 |
| Linux C++ | Ubuntu 24.04：完整无界面 C++ 数值与行为测试；标签流程额外构建 SDK 和基准 wheel，由隔离消费端检查安装后的 Python |
| Linux ASan/UBSan | 独立的手动 `Sanitizers` 工作流；Ubuntu 24.04，全量用例，不加筛选；保存命令、构建配置和 JUnit |
| Linux SDK and installed Python API | 仅标签流程：新 runner 不检出源码，校验交付散列、迁移 SDK、离线安装 wheel，运行消费端、API、示例和 C++/Python 对照 |
| Windows desktop | Windows Server 2022 / VS 2022：桌面编译、CTest 与 Qt offscreen；标签流程额外构建便携包、SDK 和基准 wheel，验证 SDK 解压迁移及 wheel 安装 |
| Python 3.11–3.15 × Windows / Linux | PR 和手动 `CI` 执行全部版本，普通 `main` 只执行基准 3.13；复用相应平台的原生依赖，独立编译扩展，运行全部 API 测试及十二个示例。基准版本同时构建 C++ 示例并验证跨语言结果。标签流程只在此运行非基准版本，构建、安装 wheel 后测试；基准 wheel 由原生及安装验证作业负责 |
| CI passed | 按本次范围检查全部预期常规作业；失败、取消或意外跳过都会失败，仅允许该范围明确计划的跳过 |

GitHub 的 `main` 分支规则应要求通过 PR 合并、**CI passed** 成功，并要求分支合并前保持最新；禁用绕过和强制推送。仓库中的规范和工作流不会自动修改 GitHub 分支保护设置。

可审阅的服务器规则配置保存在 [main.json](../../.github/rulesets/main.json) 和 [version-tags.json](../../.github/rulesets/version-tags.json)。管理员在 GitHub 应用或更新对应规则集：`main` 必须走 PR，必需的 `CI passed` 限定由 GitHub Actions 提供，合并前要求与目标分支保持最新；`v*` 标签允许新建，但禁止修改和删除。两套规则均不配置绕过名单。规则文件的修改也走 PR，文件本身不会自动调用管理 API。

Linux 和 Windows 原生作业与两平台的 Python 依赖准备在前置工具检查通过后同时启动。普通 CI 的全部 Python 版本（包括基准 3.13）只等待本平台的轻量依赖包，不等待原生编译或 HPOP 回归。原生作业不再串行重复调用基准 Python；基准作业使用自己的 C++ 示例完成跨语言对照，并在全新消费环境中运行 API 测试。依赖准备按固定 vcpkg 基线恢复或构建 ERFA、CSPICE、SQLite 等无界面依赖，每个平台只准备一次，通过同一份可复用工作流供各解释器使用。普通 `main` 同样运行两平台的基准 Python 作业并预热轻量依赖缓存。`CI passed` 要求这些作业成功，意外跳过不能放行。标签流程保留基准 wheel 与 SDK 的原生交付链，避免矩阵重复生成同一个 wheel。手动 `Sanitizers` 使用独立工作流和检查名称，其结果不能替代 PR 必需的 `CI passed`。

actionlint 在原生构建前运行，固定工具版本与 SHA-256，检查所有工作流及其 shell 脚本。更新版本时同步核验上游发布文件和校验值。

## 缓存与耗时

项目 C/C++ 编译使用固定版本的 [sccache](https://github.com/mozilla/sccache) 和 GitHub Actions 缓存；每次仍重新配置、链接并执行全部测试，只复用编译器、参数和源文件内容匹配的编译结果，不缓存测试结果或整个构建目录。Windows 使用 MSVC 2022 x64 与 Ninja Multi-Config，保留 `Release` 输出布局；Linux 使用 Ninja。原生构建与各 Python 扩展构建都启用编译缓存，任务结束记录命中、未命中及不可缓存统计。首次运行主要建立缓存，后续效果以实际命中率和阶段耗时为准。

项目使用 C++20，但没有命名模块，因此按 [CMake 建议](https://cmake.org/cmake/help/latest/manual/cmake-cxxmodules.7.html#scanning-without-modules)关闭模块依赖预扫描，保留普通头文件依赖跟踪。该设置使用目录变量，不写入 CMake 缓存。桌面程序和测试中的大型图片集合通过 Qt [`BIG_RESOURCES`](https://doc.qt.io/qt-6/qt-add-resources.html) 直接嵌入目标文件，避免把图片展开成数百 MB 的 C++ 数组再交给编译器；资源路径、内容与打包方式保持不变。

依赖使用仓库固定的 vcpkg 基线与 overlay，并缓存编译结果。Windows CI 使用专用的 `x64-windows-release` triplet，固定 v143 工具链，只构建实际用于测试和发布的 Release 依赖。本地默认 triplet 不变。首次 Windows 构建需要从源码编译 Qt、VSG、Rocky、GDAL 等依赖，耗时会明显长于后续运行。这里的 Release 是编译配置，不代表生成或发布安装包。诊断保留 14 天，跨作业依赖和标签发布产物保留 7 天。

原生、Python 依赖准备和手动 sanitizer 共用依赖缓存动作，但 Windows 桌面、Linux 原生及两平台 Python 分别使用独立缓存系列。Python 不再回退读取桌面快照，避免把 Qt、图形依赖和历史版本复制到轻量缓存。手动 sanitizer 与 Linux 原生共享相同的未插桩第三方依赖。缓存指纹包含规范化的 `vcpkg.json`、注册表配置、overlay 文件与 CI triplet；仅排除根项目的版本字段，依赖版本、功能、覆盖项和基线仍参与计算。只修改 Orbelle 版本号或 JSON 排版不会切换依赖缓存。

全部 CI 依赖安装使用 `--x-abi-tools-use-exact-versions`，由固定 vcpkg 基线中的工具清单 `scripts/vcpkg-tools.json` 决定 CMake、PowerShell 等 ABI 工具的版本，必要时下载对应工具；依据见 [vcpkg 工具版本与缓存](https://learn.microsoft.com/en-us/vcpkg/users/binarycaching-troubleshooting#mismatch-4-the-version-of-the-tools-changed-between-runs)。新缓存键不再包含 runner 镜像号，优先恢复同系列、同指纹的快照，然后尝试该系列其他快照；迁移期间可回退读取本系列旧格式键，优先同镜像旧缓存。保留原缓存路径以兼容 Actions 的路径版本校验。镜像号仍记录在诊断中；编译器不做伪装或跳过检查，vcpkg 始终按实际编译器、工具、源码及包 ABI 决定是否复用。首次切换固定工具可能需要重建，编译器更新仍会正常使相关缓存失效。

保存前按本次安装及构建计划的 ABI 文件筛选归档，只保留本次需要的版本；中断时也保留本次新完成、尚未写入安装元数据的归档。淘汰其他功能组合、历史工具版本和不完整文件，不把整个历史缓存继续累加。仅在归档新增、重建、精简或需要建立当前配置的首份快照时保存；未变化的快照不重复上传。新快照使用唯一运行编号，避免缓存不可变而丢失新增的包。安装尚未建立计划且没有新包时不覆盖已有快照。失败和取消后的保存阶段上限四分钟；runner 被强制终止或存储服务不可用时不能保证保存成功，中断时尚未完成的包仍需重新编译。

第三方依赖使用 vcpkg 的整包缓存，Linux 依赖安装明确关闭 CMake 的 sccache launcher，避免冷构建同时产生数千个重复的远程编译缓存条目。项目及 Python 扩展编译继续使用 sccache，测试范围、断言与精度容限保持不变。远端旧快照仍由 GitHub 缓存保留策略淘汰，本流程不删除其他分支的缓存。

GitHub 将 PR 生成的缓存限制在该 PR 的合并引用中，其他 PR 和 `main` 不能读取；PR 可以读取目标分支的缓存，详见[缓存访问范围](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache)。`main` 的基准测试在验证合并结果的同时维护共享依赖缓存。首次建立缓存、依赖变化或 runner 工具链变化仍可能需要较长编译，不能承诺每次都命中缓存。

排查耗时时先区分依赖安装、项目编译、测试和标签流程的打包。Actions 的 Dependency cache 摘要显示恢复的快照、精简前后归档数量与 MiB、各包安装耗时、安装前是否已有匹配 ABI、是否重编译，以及与最近似缓存版本相比变化的 ABI 输入。完整指纹和差异保存在 `before.json`、`report.json` 与 `*vcpkg_abi_info.txt` 诊断附件中；缓存中有某个 ABI 不等于安装一定成功，仍需结合 vcpkg 日志和作业结果。不要用减少 Python 支持版本或跳过行为测试来掩盖依赖冷启动问题。取消日常发布包构建可以减少重复工作，但不会消除测试所需的编译。

完整 Release 数值回归及手动 ASan/UBSan 都执行预设中的全量用例，保留原算法、参考数据、断言与精度容限；新增测试自动纳入。两者使用 `--no-tests=error` 拒绝空测试运行。

常规 CI 通过 `CTEST_PARALLEL_LEVEL=4` 在每个平台内最多并行四个测试进程，不新增分片作业或测试筛选。桌面测试共享 `orbelle_desktop_tests` 资源锁，彼此串行，仍可与数值测试并行；手动 sanitizer 保持原并发设置。CSPICE 的进程内状态不会跨 CTest 进程共享；测试只读固定参考数据，写入各自的临时目录或具名 fixture 子目录。不要同时启动多个 CTest 操作同一个构建目录。

共享桌面资源锁的测试统一设置较高的 [`COST`](https://cmake.org/cmake/help/latest/prop_test/COST.html) 调度优先级，让这条串行队列尽早与数值测试重叠，减少最后只剩桌面测试的等待。它不改变测试超时、锁、并发上限或覆盖范围；`COST` 是调度提示，不是运行时限。比较前后耗时时使用相同并发数，并确认构建目录中是否存在 CTest 学习得到的历史耗时，避免把热运行的排序优势误计为改进。

天气跳变的 HPOP 星历回归按五种积分器参数化，仍覆盖正反向、Cowell/VOP、时间正则化开关、两种报告步长、三种插值方式及跳变前后 1 ns 和边界本身。直接参考固定使用不受 VOP 开关影响的 RKV89，在每个方向内计算一次并复用；两条被测星历生成入口仍分别执行。时间范围、步长、输入、参考算法和 1 mm / 1e-6 m/s 门槛不变。其他长时天气收敛用例中的重新打开、冻结数据和新数据计算是不同的行为验证，继续保留。

太阳光压边界回归将五种积分器与两种 VOP 设置展开为十个独立 CTest 项，继续覆盖时间正则化开关、八个独立参考场景和两种步长，共 320 条轨迹。只调整调度粒度，不减少数值组合或断言；仍共享原有四进程并发额度。参考结果的复用只发生在一次测试进程内，不缓存跨提交的测试结果。

`linux-sanitizer-reports` 保存手动检查的环境、命令耗时、构建配置和 JUnit；普通 Linux 报告保存在 `linux-build-reports`。原生 Linux 上限 180 分钟，sanitizer 上限 360 分钟，以实际命令记录评估耗时；不在同一构建目录中并发运行有共享文件的测试。

常规 Linux 与手动 sanitizer 作业独立恢复同一类 vcpkg 二进制依赖缓存。ASan 保留内存泄漏检测，ASan/UBSan 均遇错失败。

Windows、Linux 和 Python 的 `commands.json` 记录每条命令的 UTC 开始时间、耗时与退出码，Actions 作业摘要展示分阶段用时，失败命令同样保留。父任务中的 Python 总耗时包含其嵌套报告，不能跨表重复求和。依赖缓存命中率和安装时间查看对应 Actions 步骤；项目编译缓存需根据这些记录再评估，当前不缓存项目构建目录。

本地日常开发只需验证 Python 3.13；多版本兼容性由 PR CI 负责，不必在开发机安装多套 Python。`pyproject.toml` 的 `[tool.orbelle.ci]` 统一定义基准版本和测试版本，CI 矩阵与发布附件检查共用这份配置。其他解释器版本只编译 Python 扩展，标签流程再构建对应 wheel。

普通托管 runner 不承担真实 Vulkan GPU 验收；offscreen 测试通过不代表三维呈现或性能达标。带 GPU 的验证仍按[构建说明](build.md)执行。联网 IERS 服务检查是可选测试，CI 不依赖实时数据服务。

## 发布内容

| 文件 | 用途 |
| --- | --- |
| `Orbelle-X.Y.Z-win-x64.zip` | Windows x64 便携桌面，解压后运行 `Orbelle.exe` |
| `Orbelle-X.Y.Z-win-x64-{app,runtime,resources}.zip` | Windows 自动更新分包，分别包含程序、依赖与资源 |
| `Orbelle-X.Y.Z-win-x64-update.json` | 分包和完整版本的文件、大小及 SHA-256 清单 |
| `Orbelle-X.Y.Z-sdk-win-x64.zip` | Windows x64 / VS 2022 Release C++ 无界面 SDK |
| `Orbelle-X.Y.Z-sdk-linux-x64.tar.gz` | Ubuntu 24.04 x86_64 Release C++ 无界面 SDK |
| `orbelle-X.Y.Z-cpNNN-cpNNN-win_amd64.whl` | Windows x64；CPython 3.11～3.15，每个版本一个 wheel |
| `orbelle-X.Y.Z-cpNNN-cpNNN-linux_x86_64.whl` | Ubuntu 24.04 x86_64；CPython 3.11～3.15，每个版本一个 wheel |
| `SHA256SUMS` | 上述十七个文件的 SHA-256 |

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
git tag -a v0.1.1 -m "Orbelle v0.1.1"
git push origin v0.1.1
```

触发条件是推送 **Git 标签**，提交信息写 `v0.1.1` 不会发布。三个清单的版本必须相同，并与标签精确匹配；`v1.2`、`v01.2.3`、`v1.2.3-rc1` 等格式会在构建前被拒绝。当前流程只处理稳定的三段版本号。

标签必须指向已经进入 `origin/main` 历史的提交，检出的提交必须与标签一致；允许发布 main 中较早的提交，但拒绝未合并的主题分支提交。版本和归属在构建前检查，公开 Release 前再次核验；找不到标签或 main 历史时停止。不要通过重新打标签修复已发布版本，应提交修复 PR 并使用新版本号。

发布作业仅使用本次标签构建产生的文件，核对桌面、两套 SDK、完整 Python 矩阵的 wheel 及其版本后计算校验值，先创建草稿，上传并核对全部附件，最后公开。说明包含平台要求与 GitHub 自动生成的变更记录。上传失败会留下未公开的草稿，可在 Actions 中重新运行失败作业；已经公开的同名版本不会被覆盖，应修复后发布新的版本号。

工作流只需 GitHub 自动提供的 `GITHUB_TOKEN`，不需要额外 PAT。测试作业只有读取权限，只有发布作业具有 `contents: write`；Actions 固定到完整提交 SHA，Dependabot 每月检查更新。
