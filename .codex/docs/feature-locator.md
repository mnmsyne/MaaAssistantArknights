# MAA 功能与界面定位

本文是从用户可见名称定位代码的唯一功能索引。表内路径分别相对 `src/MaaWpfGui/`、`src/MaaCore/` 和 `resource/`；通用分层和目录职责见 [仓库与架构定位](architecture.md)。

## 顶层界面

| 界面     | View                          | ViewModel                             | 主要职责                                 |
| -------- | ----------------------------- | ------------------------------------- | ---------------------------------------- |
| 根窗口   | `Views/UI/RootView.xaml`      | `ViewModels/UI/RootViewModel.cs`      | 页面导航、资源包导入和全局提示           |
| 一键长草 | `Views/UI/TaskQueueView.xaml` | `ViewModels/UI/TaskQueueViewModel.cs` | 持久化任务队列、顺序执行、日志和任务状态 |
| 自动战斗 | `Views/UI/CopilotView.xaml`   | `ViewModels/UI/CopilotViewModel.cs`   | 作业加载、校验、编队和 Copilot 类任务    |
| 小工具   | `Views/UI/ToolboxView.xaml`   | `ViewModels/UI/ToolboxViewModel.cs`   | 一次性识别、截图预览和资源化工具         |
| 设置     | `Views/UI/SettingsView.xaml`  | `ViewModels/UI/SettingsViewModel.cs`  | 连接、资源、更新、外观和全局行为         |
| 运行浮层 | `Views/UI/OverlayWindow.xaml` | `ViewModels/UI/OverlayViewModel.cs`   | 运行状态、日志和快捷控制                 |

WPF 顶层页面使用 `Main/AsstProxy.cs` 调用 Core，并共享 `States/RunningState.cs`。页面文字从 `Res/Localizations/` 定位，不要用显示文案猜测任务类型或配置键。

## 一键长草

队列从 `Configuration/Single/SpecificConfig.cs` 持久化多态 `MaaTask/BaseTask`。每个设置 ViewModel 的 `SerializeTask()` 生成 `Models/AsstTasks/` 参数，由 `TaskQueueViewModel.LinkStartWithTasks()` 依次追加并统一启动。机制细节见 [WPF 与 Core 桥接](wpf-core-bridge.md)。

| 界面功能   | WPF 子页面、配置与序列化                                                                                                  | Core 入口                                                                                   | 深入定位与资源                                                                 |
| ---------- | ------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| 开始唤醒   | `StartUpTaskUserControl.xaml` → `StartUpSettingsUserControlModel` → `StartUpTask` / `AsstStartUpTask`                     | `Task/Interface/StartUpTask.*`、`Task/Miscellaneous/AccountSwitchTask.*`                    | 启动客户端、进入主页、账号切换和账号相关资源任务                               |
| 理智作战   | `FightSettingsUserControl.xaml` → `FightSettingsUserControlModel` → `FightTask` / `AsstFightTask`                         | `Task/Interface/FightTask.*`、`Task/Fight/`                                                 | `tasks/tasks.json` 主域作战任务、`tasks/Stages/`、掉落识别与关卡导航           |
| 基建换班   | `InfrastSettingsUserControl.xaml` → `InfrastSettingsUserControlModel` → `InfrastTask` / `AsstInfrastTask`                 | `Task/Interface/InfrastTask.*`、`Task/Infrast/`                                             | `Vision/Infrast/`、`custom_infrast/`、基建设施配置                             |
| 自动公招   | `RecruitSettingsUserControl.xaml` → `RecruitSettingsUserControlModel` → `RecruitTask` / `AsstRecruitTask`                 | `Task/Interface/RecruitTask.*`、`Task/Miscellaneous/AutoRecruitTask.*`                      | `recruitment.json`、OCR 与公招结果回调                                         |
| 信用收支   | `MallSettingsUserControl.xaml` → `MallSettingsUserControlModel` → `MallTask` / `AsstMallTask`                             | `Task/Interface/MallTask.*`、`Task/Miscellaneous/CreditFightTask.*`、`CreditShoppingTask.*` | 商城资源任务；信用作战会复用 `CopilotTask`                                     |
| 领取奖励   | `AwardSettingsUserControl.xaml` → `AwardSettingsUserControlModel` → `AwardTask` / `AsstAwardTask`                         | `Task/Interface/AwardTask.*`                                                                | 邮件、任务、签到等领取资源任务                                                 |
| 自动肉鸽   | `RoguelikeSettingsUserControl.xaml` → `RoguelikeSettingsUserControlModel` → `RoguelikeTask` / `AsstRoguelikeTask`         | `Task/Interface/RoguelikeTask.*`、`Task/Roguelike/`                                         | `Vision/Roguelike/`、`roguelike/` 策略与主题资源                               |
| 生息演算   | `ReclamationSettingsUserControl.xaml` → `ReclamationSettingsUserControlModel` → `ReclamationTask` / `AsstReclamationTask` | `Task/Interface/ReclamationTask.*`、`Task/Reclamation/`                                     | `tasks/RA/` 与生息演算插件                                                     |
| 更新数据   | `UserDataUpdateSettingsUserControl.xaml` → `UserDataUpdateSettingsUserControlModel` → `UserDataUpdateTask`                | 组合 `OperBox` 与 `Depot`                                                                   | WPF 按上次同步时间决定追加项；没有 Core `UserDataUpdate` 接口                  |
| 库存保持   | `DepotMaintainTaskUserControl.xaml` → `DepotMaintainTaskUserControlModel` → `DepotMaintainTask`                           | 可先追加 `Depot`，再追加多个 `Fight`                                                        | WPF 按仓库缺口、关卡开放状态和库存目标生成计划；没有 Core `DepotMaintain` 接口 |
| 自定义任务 | `CustomUserControl.xaml` → `CustomSettingsUserControlModel` → `CustomTask` / `AsstCustomTask`                             | `Task/Interface/CustomTask.*`                                                               | 执行指定资源任务图，承载短期或单用途流程                                       |
| 完成后动作 | `PostActionUserControl.xaml`、`Models/PostActionSetting.cs` → `AsstCloseDownTask`                                         | `Task/Interface/CloseDownTask.*`、`AsstProxy.AsstAppendCloseDown()`                         | 队列结束后的关闭游戏客户端、退出模拟器/MAA、休眠关机等；不是 `MaaTask` 队列项  |

修改现有选项时沿“设置 XAML → `*SettingsUserControlModel` → `MaaTask/*Task.cs` → `AsstTasks/Asst*Task.cs` → Core `set_params()`”定位。新增队列项还要注册 `TaskQueueViewModel.TaskTypeList`、处理旧配置迁移、任务 ID、回调和本地化。

## 自动战斗

`CopilotViewModel` 当前只接收 JSON 作业，并解析本地文件、缓存、`maa://`、`prts://` 作业或作业集。主要参数模型是 `Models/AsstTasks/AsstCopilotTask.cs` 和 `AsstParadoxCopilotTask.cs`；旧 `VideoRecognition` 启动路径在 WPF 已停用，Core 侧仍注册该任务类型。

| 页签               | WPF 关键入口                                     | Core 任务        | 执行模块                                                                            |
| ------------------ | ------------------------------------------------ | ---------------- | ----------------------------------------------------------------------------------- |
| 主线、故事集、支线 | `ParseCopilotAsync()`、`AddCopilotTaskToList()`  | `Copilot`        | `CopilotTask`、`BattleFormationTask`、`BattleProcessTask`、`MultiCopilotTaskPlugin` |
| 保全派驻           | `ParseSSSCopilot()`                              | `SSSCopilot`     | `SSSCopilotTask`、`SSSStageManagerTask`、`SSSBattleProcessTask`                     |
| 悖论模拟           | `VerifyParadoxTasks()`、`AsstParadoxCopilotTask` | `ParadoxCopilot` | `ParadoxCopilotTask`、`ParadoxRecognitionTask`、`BattleProcessTask`                 |
| 其他活动           | 普通作业解析，不启用作业列表导航                 | `Copilot`        | `CopilotTask`、`BattleProcessTask` 和作业指定的资源导航                             |

启动链为 `Start()` → `ValidateStartAsync()` → 连接设备 → `AppendAndStartCopilotAsync()`。作业动作由 `BattleProcessTask` 推进，战场事实来自 `Vision/Battle/` 与 `BattleHelper`，设备操作进入 `Controller/`。

## 小工具

小工具通常由 `ToolboxViewModel` 直接追加一次性任务并启动，不进入一键长草队列。Core 回调经 `AsstProxy` 分发给对应解析方法。

| 页签     | WPF 关键入口                                       | Core、C API 或资源入口                                                                 | 结果与配置                                                                      |
| -------- | -------------------------------------------------- | -------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| 公招识别 | `RecruitStartCalc()`、`ProcRecruitMsg()`           | `Recruit` 计算模式、`RecruitTask`                                                      | `UpdateRecruitResult()` 生成标签组合；可结合干员潜能数据                        |
| 干员识别 | `StartOperBox()`、`OperBoxParse()`                 | `OperBoxTask`、`AsstStartOperBox()`                                                    | 持有、未持有、等级、潜能和导出                                                  |
| 仓库识别 | `StartDepot()`、`DepotParse()`                     | `DepotTask`、`AsstStartDepot()`                                                        | 材料数量、同步时间、导出和作战掉落增量更新                                      |
| 牛牛抽卡 | `GachaOnce()`、`GachaTenTimes()`、`StartGacha()`   | `Custom`：`GachaOnce` 或 `GachaTenTimes`                                               | 会执行真实寻访操作，确认提示是安全边界                                          |
| 牛牛监控 | `Peep()`、`RefreshPeepImageAsync()`                | 原生 `AsstGetImageBgr`（WPF 封装 `AsstProxy.AsstGetImageBgrData`）强制截图，不追加任务 | 定时器、信号量、数组池和目标 FPS；可在其他任务运行时预览                        |
| 牛杂     | `UpdateMiniGameTaskList()`、`StartMiniGameAsync()` | `Custom` 资源任务；像素画经 `AsstProxy.AsstPixelPaint()` 进入 `Task/MiniGame/` 插件    | 常驻条目在 `StageManager`，活动条目来自 Maa API 缓存 `gui/StageActivityV2.json` |

牛杂动作主要位于 `tasks/MiniGame/`。常驻商店与隐藏战线入口由 `Services/StageManager.InitializeDefaultMiniGameEntries()` 提供；活动条目必须同时存在可解析的活动配置和对应资源任务。

## 设置页功能

设置页除连接、资源和版本更新外还承载以下稳定功能，入口均在 `Views/UserControl/Settings/` 与对应 ViewModel。

| 功能       | 主要入口                                                                         | 说明                                             |
| ---------- | -------------------------------------------------------------------------------- | ------------------------------------------------ |
| 定时启动   | `TimerSettingsUserControl.xaml`、`Configuration/Global/Timer.cs`                 | 按配置时刻自动执行一键长草队列                   |
| 多配置管理 | `ConfigurationMgrUserControl.xaml`                                               | `ConfigFactory.Root.Configurations` 的增删与切换 |
| 远程控制   | `RemoteControlUserControl.xaml`、`Services/RemoteControl/`                       | 外部接口查询与下发任务                           |
| 外部通知   | `ExternalNotificationSettingsUserControl.xaml`、`Services/ExternalNotification/` | 任务事件推送到外部渠道                           |
| 全局热键   | `HotKeySettingsUserControl.xaml`、`Services/HotKeys/`                            | 系统级快捷键绑定                                 |

## 其他常见入口

| 目标             | 首要入口                                                                                            | 常见联动                                                    |
| ---------------- | --------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| 模拟器连接       | WPF `ViewModels/UserControl/Settings/ConnectSettingsUserControlModel.cs`、`AsstProxy.AsstConnect()` | Core `Controller/`、连接配置和错误回调                      |
| 资源加载与热更新 | `AsstProxy`、`Models/ResourceUpdater.cs`                                                            | Core `ResourceLoader`、资源缓存和差异覆盖                   |
| 应用版本更新     | `VersionUpdateSettingsUserControlModel`、更新对话框                                                 | `src/MaaUpdater/`、下载和退出重启流程                       |
| 关卡开放信息     | `Services/StageManager.cs`                                                                          | Maa API 缓存、`tasks/Stages/`、理智作战和库存保持           |
| 本地化           | `Res/Localizations/`、`LocalizationHelper`                                                          | 可见文案、枚举显示和值的稳定映射                            |
| 回调显示异常     | `Main/AsstProxy.cs` 回调分派                                                                        | `taskchain`、`taskid`、`what`、UI Dispatcher 和页面解析方法 |

## 快速判断修改层

- 只改页面布局或文案：`Views/`、`Res/Localizations/`，业务状态仍留在 ViewModel。
- 只改用户配置和编排：`Configuration/`、ViewModel、`Models/AsstTasks/`，优先组合已有 Core 任务。
- 只改识别或操作图：`resource/tasks/`、`template/`、OCR 或模型，检查所有服务器和平台覆盖。
- 改复杂运行时判断：Core 领域任务或插件，复用 `Vision` 和 `Controller`。
- 多个调用方都需要新能力：新增 `Task/Interface/` 公共任务，按 [MaaCore 任务接口](core-task-interface.md) 检查契约。
