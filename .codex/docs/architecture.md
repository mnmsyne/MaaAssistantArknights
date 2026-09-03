# MAA 仓库与架构定位

本文是 `.codex/docs/` 的入口，只回答“代码属于哪一层、应先看哪里”。局部实现仍应先读取目标目录最近的 `AGENTS.md`。

## 按问题选择文档

| 想解决的问题                                       | 读取文档                                               |
| -------------------------------------------------- | ------------------------------------------------------ |
| 从界面或功能名找到 WPF、Core、resource 代码        | [功能与界面定位](feature-locator.md)                   |
| 理解 WPF 配置、序列化、任务 ID、回调和资源生命周期 | [MaaWpfGui 与 MaaCore 桥接](wpf-core-bridge.md)        |
| 新增或修改公共任务类型、参数与回调协议             | [MaaCore 任务接口](core-task-interface.md)             |
| 确认游戏术语、UI 文案与代码字段对应关系            | [明日方舟术语与仓库字段对照](arknights-terminology.md) |
| 同步个人分支、提交或准备 PR                        | [Git 工作流](git-workflow.md)                          |

## 仓库模块

| 目录                         | 主要职责                                      |
| ---------------------------- | --------------------------------------------- |
| `include/`                   | 稳定的公开 C API                              |
| `src/MaaCore/`               | C++20 核心：任务、识别、控制器和资源加载      |
| `src/MaaWpfGui/`             | Windows WPF 客户端：界面、配置和客户端编排    |
| `resource/`                  | 任务图、模板、OCR、模型和服务器、平台差异资源 |
| `tools/`                     | 开发、资源维护、发布和诊断辅助工具            |
| `src/Cpp/`、`src/Python/` 等 | 语言绑定或调用示例                            |
| `src/MaaUpdater/`            | 更新组件                                      |
| `unit_test/`                 | MaaCore 纯算法测试                            |

`3rdparty/EmulatorExtras/`、`src/MaaUtils/`、`src/MaaMacGui/`、`src/MAAUnified/`、`src/maa-cli/` 和 `test/` 是 Git 子模块，默认只读。只有用户明确要求时才修改其内容或 gitlink，并先读取子模块自己的约定。

## 运行主线

```text
WPF / 语言绑定 / 第三方调用方
  -> include/AsstCaller.h
  -> src/MaaCore/AsstCaller.cpp
  -> Assistant：连接、任务队列、工作线程、回调队列
  -> Task/Interface：公共任务参数边界
  -> PackageTask / ProcessTask / 领域任务与插件
  -> Vision 识别 + Controller 设备交互
  -> resource 任务图、模板、OCR、模型和差异覆盖
```

界面只处理展示、输入和客户端编排；`Vision/` 只提取画面事实；`Controller/` 只处理设备 I/O；业务决策和状态推进属于任务层。

## MaaCore 目录

| 目录或文件                                            | 首要职责                                                    |
| ----------------------------------------------------- | ----------------------------------------------------------- |
| `AsstCaller.cpp`、`Assistant.*`                       | C API 落地、实例、连接、任务队列、工作线程和回调队列        |
| `Task/Interface/`                                     | `StartUp`、`Fight`、`Copilot` 等公共任务类型及 `set_params` |
| `Task/Miscellaneous/`                                 | 编队、战斗、公招等可复用任务                                |
| `Task/Fight/`、`Task/Infrast/`                        | 理智作战与基建领域状态机                                    |
| `Task/Roguelike/`、`Task/Reclamation/`、`Task/SSS/`   | 肉鸽、生息演算和保全派驻业务流程                            |
| `Task/MiniGame/`、`Task/Experiment/`                  | 小游戏插件（像素画、隐秘战线）与实验性识别、战斗任务        |
| `Ui/`                                                 | 编队界面的自动化抽象（快速编队、助战列表），位于顶层        |
| `Vision/`                                             | 通用和领域识别器                                            |
| `Controller/`                                         | ADB、窗口、触控、截图和设备生命周期抽象                     |
| `Config/TaskData.*`                                   | 合并、解析和生成资源任务图                                  |
| `Config/ResourceLoader.*`                             | 加载任务、模板、OCR、模型和领域配置                         |
| `Config/OnnxSessions.*`、`Config/GpuDeviceSelector.*` | 模型会话管理与 GPU/NCNN 推理后端选择                        |
| `Common/`、`Utils/`                                   | 日志、类型、文件、JSON 和通用基础设施                       |

## Resource 目录

| 目录或文件                                               | 首要职责                                                                                 |
| -------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| `tasks/tasks.json`                                       | 主域任务图，承载 `Fight`、`Infrast`、`Mall`、`Award`、`Recruit`、`StartUp`、`Copilot` 等 |
| `tasks/<domain>/`                                        | 按领域拆分的任务图：`Stages`、`Roguelike`、`RA`、`MiniGame`、`UiTheme`、`Copilot`        |
| `template/`                                              | 模板匹配图片；文件名与任务配置形成字符串契约                                             |
| `PaddleOCR/`、`PaddleCharOCR/`                           | OCR 模型和字典                                                                           |
| `onnx/`                                                  | 战斗、方向和干员等识别模型                                                               |
| `global/<server>/resource/`                              | 外服覆盖或补充                                                                           |
| `platform_diff/`                                         | 平台和窗口绑定模式差异                                                                   |
| `roguelike/`、`custom_infrast/`                          | 肉鸽策略与自定义基建配置                                                                 |
| `Arknights-Tile-Pos/`、`copilot/`                        | 地图格点和随包作业数据                                                                   |
| `config.json`、`recruitment.json`、`battle_data.json` 等 | Core 领域配置                                                                            |

资源以主资源、缓存、服务器差异、服务器缓存、平台差异的顺序叠加。文件边界不等于运行时任务边界；`TaskData` 还会处理 `baseTask`、`@` 派生名和任务跳转。仓库不再携带根级 `tasks.json`，Core 仅在加载旧版外部资源包时兼容读取并告警。

## MaaWpfGui 目录

| 目录或文件                                      | 首要职责                                          |
| ----------------------------------------------- | ------------------------------------------------- |
| `Main/Bootstrapper.cs`                          | 启动、单实例、IoC、根窗口和退出生命周期           |
| `Main/AsstProxy.cs`                             | Core 资源、句柄、连接、任务调用和回调分派门面     |
| `Services/MaaService.cs`                        | `MaaCore.dll` 公共 C API 的 P/Invoke 声明         |
| `Services/`                                     | 跨页面及外部系统能力，如活动关卡和 API 缓存       |
| `Views/UI/`、`ViewModels/UI/`                   | 顶层页面及其状态、命令和客户端编排                |
| `Views/UserControl/`、`ViewModels/UserControl/` | 页面子模块和设置面板                              |
| `Configuration/`                                | Root、Global、Specific 配置、迁移和持久化任务模型 |
| `Models/AsstTasks/`                             | 一次 Core 请求的任务类型和 JSON 参数模型          |
| `States/RunningState.cs`                        | 页面间运行互斥、停止和空闲状态                    |
| `Res/`、`Styles/`                               | 本地化、主题、图标和展示资源                      |

具体页面、按钮和子功能只在 [功能与界面定位](feature-locator.md) 维护，避免在架构文档重复功能清单。

## Tools 目录

| 类别         | 代表目录                                                                                |
| ------------ | --------------------------------------------------------------------------------------- |
| 图片与模板   | `ImageCropper`、`GetImageFromROI`、`MaskRangeTool`、`OptimizeTemplates`、`SyncTemplate` |
| 资源与多语言 | `ResourceUpdater`、`OverseasClients`、`TaskSorter`、肉鸽相关工具                        |
| 发布与统计   | `ChangelogGenerator`、`ReleaseDownloadStats`、`OTAPacker`、`AppImage`                   |
| 开发验证     | `SmokeTesting`、`ClangFormatter` 和根目录构建辅助脚本                                   |

各工具通常相互独立，可能批量改写资源、依赖特定工作目录或访问网络。修改或执行前读取 `tools/AGENTS.md` 和具体入口；输出落入其他目录时继续遵循目标目录的规则。

## 按变更选择入口

| 变更目标            | 首要入口                                    | 必查联动                             |
| ------------------- | ------------------------------------------- | ------------------------------------ |
| 修改用户可见功能    | [功能与界面定位](feature-locator.md)        | 对应 WPF、Core、resource 和本地化    |
| 新增公共任务类型    | `Assistant::append_task`、`Task/Interface/` | 参数模型、回调、资源和语言绑定       |
| 修改资源流水线      | `resource/tasks/`                           | `TaskData`、字符串引用和差异覆盖     |
| 调整识别            | `Vision/`、`resource/template/`             | 截图样本、OCR 或模型配置             |
| 调整设备交互        | `Controller/`                               | 连接设置、跨平台后端和错误恢复       |
| 修改 WPF 配置或队列 | `Configuration/`、相关 ViewModel            | 序列化、迁移、任务 ID 和回调         |
| 修改 C API          | `include/AsstCaller.h`、`AsstCaller.cpp`    | `MaaService`、全部语言绑定和协议文档 |
| 修改开发工具        | `tools/<tool>/`                             | 输入输出、资源契约和平台脚本         |
