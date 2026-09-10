# Tools 修改约定

本文件只补充 `tools/` 的局部规则，继承根目录 `AGENTS.md`。下文短路径相对此目录，`resource/`、`src/` 和 `.codex/` 等路径相对仓库根目录。

## 快速定位

| 类别         | 代表入口                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------- |
| 图片与模板   | `ImageCropper/`、`GetImageFromROI/`、`MaskRangeTool/`、`OptimizeTemplates/`、`SyncTemplate/` |
| 资源与多语言 | `ResourceUpdater/`、`OverseasClients/`、`TaskSorter/`、肉鸽相关工具                          |
| 发布与统计   | `ChangelogGenerator/`、`ReleaseDownloadStats/`、`OTAPacker/`、`AppImage/`                    |
| 开发验证     | `SmokeTesting/`、`ClangFormatter/` 和根目录构建辅助脚本                                      |

各子目录通常是独立工具，不是 MaaCore 或 WPF 的稳定运行时 API。修改前先读入口文件、README、依赖清单和相邻平台脚本，不假设工具间共享环境或调用约定。

## 输入、输出与副作用

- 运行工具前确认默认工作目录、输入范围、输出位置、覆盖方式、网络访问和外部命令；很多工具会直接批量改写 `resource/`。
- 对批量改写提供或保留明确的范围控制；除非用户明确要求，不把单文件任务扩大到整个资源树，也不顺手格式化无关输出。
- 输出应可重复：使用稳定排序、编码和换行，不写入用户名、盘符、绝对路径、当前时间或随机顺序，除非目标格式明确要求。
- 路径使用仓库相对位置或显式参数，不依赖个人目录。缓存、虚拟环境、下载内容、构建目录和临时文件留在既有忽略范围。
- 下载数据、调用远端 API、上传结果、发布或执行完整烟雾测试前，先说明目标与副作用并取得对应授权；令牌、Cookie 和账号信息不得写入仓库或日志。
- 工具输出落入 `resource/`、Core、WPF 或发布配置时，继续读取目标目录最近的 `AGENTS.md`，并按目标文件规则审查和验证。

## 实现原则

- 保持单一职责和现有 CLI、拖放、批处理或脚本调用方式；不要为少量复用引入跨工具隐式状态或重量级共享框架。
- Python 工具使用各自 `requirements.txt` 或已有环境约定，不把局部依赖无意提升为全仓依赖。
- JSON、YAML、图片和模板使用解析器或既有库生成，保持字段、排序、色彩、尺寸和命名约定，不用字符串拼接模拟结构化格式。
- Windows、macOS 和 Linux 并行脚本保持参数、退出码和输出语义一致；只修改一个平台时说明其他平台为何不受影响。
- PowerShell 脚本包含中文字符串时保存为 UTF-8 with BOM，避免名称匹配和参数传递静默失败。
- `ResourceUpdater/` 的根 CMake 目标是 `res_updater`。生成资源时审查生成器和全部输出 diff，不直接维护下一次运行会覆盖的派生文件。

## 验证

- 对修改文件运行 `pre-commit run --files <paths>`；Python 使用 Ruff，C++ 使用根 `.clang-format`，JSON 和 YAML 使用 Prettier。
- 先用最小样例或只读模式检查参数、错误处理、退出码和输出结构，再执行授权范围内的真实写入。
- 批量生成后检查 `git status`、完整 diff、未预期文件、稳定排序和重复运行是否产生额外变化。
- `ResourceUpdater/` 需要构建时使用目标 `res_updater`；生成结果还要按 `resource/AGENTS.md` 检查引用、覆盖和格式。
