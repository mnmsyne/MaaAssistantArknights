# MaaWpfGui 修改约定

本文件只补充 `src/MaaWpfGui/` 的局部规则，继承根目录 `AGENTS.md`。下文短路径相对此目录，`.codex/`、`resource/` 和 `src/MaaCore/` 等路径相对仓库根目录。

## 快速定位

| 位置                     | 职责                                              |
| ------------------------ | ------------------------------------------------- |
| `Main/Bootstrapper.cs`   | 启动、单实例、IoC、根窗口和退出生命周期           |
| `Main/AsstProxy.cs`      | Core 资源、句柄、连接、任务调用和回调分派门面     |
| `Services/MaaService.cs` | `MaaCore.dll` 公共 C API 的 P/Invoke 声明         |
| `Views/`、`ViewModels/`  | 视图与页面状态、命令和客户端编排                  |
| `Configuration/`         | Root、Global、Specific 配置、迁移和持久化任务模型 |
| `Models/AsstTasks/`      | 单次 Core 请求的任务类型和 JSON 参数              |
| `Services/`              | 跨页面、文件、网络和外部系统能力                  |
| `States/RunningState.cs` | 页面间运行互斥、停止和空闲状态                    |
| `Res/`、`Styles/`        | 本地化、主题、图标和展示资源                      |

从页面或功能名定位代码时使用 `../../.codex/docs/feature-locator.md`；配置、序列化、回调和资源生命周期见 `../../.codex/docs/wpf-core-bridge.md`。

## MVVM 与线程边界

- 本项目使用 `net10.0-windows`、C# 14 和现有 MVVM 结构。遵循本目录 `.editorconfig`、StyleCop、可空性和相邻异步模式。
- `Views/` 与 `Res/` 只承载展示和绑定；业务状态进入 ViewModel，跨页面或外部能力进入 Service，持久化和请求契约进入 `Configuration/` 或 `Models/`。
- code-behind 只处理难以绑定的纯 UI 事件、布局和控件互操作，不复制校验、配置、Core 调用或业务编排。
- Core、网络和文件操作不得阻塞 UI 线程。Core 回调来自后台线程，属性和可观察集合通过现有 Dispatcher 或 `Execute.OnUIThread` 更新。
- 新增依赖沿 `Bootstrapper.ConfigureIoC` 和 `Instances` 的既有生命周期接入，不在 ViewModel 临时创建新的全局 Service。

## 配置与 Core 调用

- `ConfigFactory.Root`、`ConfigFactory.CurrentConfig` 及其 `Configurations` 是配置权威来源；`ConfigurationHelper` 只用于启动引导、旧格式迁移和少量兼容键，不建立双向补全、双写或第二份配置列表。
- 持久化 `BaseTask` 保存长期用户设置；`AsstBaseTask` 只描述一次 Core 请求。运行时任务 ID、临时识别结果和页面忙碌状态不持久化。
- 一键长草沿 `BaseTask` → TaskSettings ViewModel → `AsstBaseTask` → `AsstProxy` 接入，并保存返回的一个或多个任务 ID。客户端组合任务优先复用已有 Core 能力。
- 自动战斗和小工具通常追加即时任务，不写入常规任务队列；仍须使用共享 `RunningState` 并覆盖连接、启动、停止和失败恢复。
- `AsstTaskType` 是发送给 Core 的公共类型字符串；`AsstProxy.TaskType` 是 WPF 内部页面与日志分类。两者不能混用，一个 WPF 类型可以映射到多个 Core 请求。
- 回调先按 `taskid` 与 `taskchain` 定位请求，再解释稳定 `what` 和 `details`；不得只按任务类型更新页面，也不得让晚到回调污染新请求。

## 生命周期与功能组织

- 普通启动、Core 资源热更新和应用版本更新是独立生命周期。分别沿 `Bootstrapper`、`AsstProxy` / `Models/ResourceUpdater.cs`、版本更新 ViewModel 与对话框定位，不合并状态或恢复路径。
- 页面只选择资源上下文，不按客户端名称复制 Core 或 resource 已表达的能力判断；资源重载只在安全状态执行。
- 一次性识别或操作进入“小工具”。新增非平凡工具时拆分独立子 ViewModel、UserControl 和必要 Service，不继续扩大集中式 `ToolboxViewModel`。
- 用户可见文案走现有本地化机制；协议键、模型 ID、枚举值和任务类型保留稳定英文。可见行为变化同步评估设置页、提示和知识文档。
- 不编辑 `obj/`、`bin/` 或生成的 XAML、资源和构建产物。

## 验证

- 对修改文件运行 `pre-commit run --files <paths>`。
- 定向构建使用 `dotnet build src/MaaWpfGui/MaaWpfGui.csproj -c Debug -p:Platform=x64`。
- XAML 或交互变化除构建外还要说明手工验证路径；配置变化至少覆盖空配置、已有配置和配置切换。
- Core 联动检查任务类型、参数拒绝、任务 ID、回调线程、停止与晚到回调；需要完整依赖链时使用根目录 Windows Debug x64 CMake 预设。
