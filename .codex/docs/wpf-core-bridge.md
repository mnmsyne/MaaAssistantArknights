# MaaWpfGui 与 MaaCore 桥接

本文只说明 WPF 如何持久化配置、构造 Core 请求、管理运行状态和消费回调。具体界面功能与代码入口见 [功能与界面定位](feature-locator.md)，公共任务协议见 [MaaCore 任务接口](core-task-interface.md)。

## 桥接层

| 层            | 主要入口                                    | 责任                                              |
| ------------- | ------------------------------------------- | ------------------------------------------------- |
| View          | `Views/UI/`、`Views/UserControl/`           | 展示、绑定和输入，不承载业务状态机                |
| ViewModel     | `ViewModels/UI/`、`ViewModels/UserControl/` | 页面状态、命令、校验和客户端编排                  |
| 持久化配置    | `Configuration/`                            | Root、Global、Specific 作用域、迁移和长期任务设置 |
| Core 请求模型 | `Models/AsstTasks/`                         | 单次请求的任务类型与 JSON 参数                    |
| WPF 门面      | `Main/AsstProxy.cs`                         | 资源、句柄、连接、任务追加、启动停止和回调分派    |
| 原生声明      | `Services/MaaService.cs`                    | `MaaCore.dll` 公共 C API 的 P/Invoke 签名         |
| 共享状态      | `States/RunningState.cs`                    | 页面间运行互斥、停止过程和空闲等待                |

`MaaService` 只随原生函数签名变化；页面通常只调用 `AsstProxy`，不直接创建 Core 句柄或复制 P/Invoke 调用。

## 配置与请求模型

`ConfigFactory.Root` 和 `ConfigFactory.CurrentConfig` 是当前配置的权威来源。Root 保存应用级结构，Global 保存跨配置设置，Specific 保存当前配置的任务与选项。`ConfigurationHelper` 主要用于启动引导、旧格式迁移和少量兼容读写，不应成为第二份运行时真相。

持久化模型与 Core 请求必须分开：

| 模型                                    | 保存内容                                     | 不应保存                                    |
| --------------------------------------- | -------------------------------------------- | ------------------------------------------- |
| `Configuration/Single/MaaTask/BaseTask` | 用户长期设置、启用状态、顺序和客户端编排信息 | 本次运行任务 ID、临时识别结果和页面忙碌状态 |
| `Models/AsstTasks/AsstBaseTask`         | 一次 Core 请求所需的稳定任务类型和 JSON 参数 | UI 展示状态和配置迁移信息                   |

新增设置时先确定 Root、Global 或 Specific 作用域，再检查默认值、旧配置迁移、配置切换通知和序列化兼容。

## 三种执行模式

| 模式         | 适用入口                       | 特征                                                            |
| ------------ | ------------------------------ | --------------------------------------------------------------- |
| 持久化队列   | 一键长草                       | 多个 `BaseTask` 经设置 ViewModel 序列化，保存任务 ID 后统一启动 |
| 页面即时任务 | 自动战斗、公招识别、仓库识别等 | 页面连接设备，追加当前请求并立即启动，不写入常规任务队列        |
| 纯客户端能力 | 文件处理、部分预览和配置工具   | 不追加 Core 任务；如需截图仍通过 `AsstProxy` 使用现有 Core 句柄 |

三种模式共享 `RunningState`。连接、追加或启动失败时必须恢复空闲状态；停止期间不得由另一页面绕过互斥重新启动。

## 一键长草序列化

```text
SpecificConfig.TaskQueue 中的 BaseTask
  -> 对应 TaskSettingsViewModel.SerializeTask()
  -> AsstBaseTask.Serialize()
  -> AsstProxy.AsstAppendTaskWithEncoding()
  -> 原生 AsstAppendTask()
  -> 返回 taskid 并绑定到本次 TaskItem
  -> 全部追加后 AsstStart()
```

关键入口：

1. `TaskQueueViewModel.LinkStart()` 获取任务序列化锁。
2. `LinkStartWithTasks()` 检查版本、连接设备并遍历当前 `TaskQueue`。
3. `SerializeTask()` 将每个 `BaseTask` 分发给已注册的设置 ViewModel。
4. 设置 ViewModel 可以生成一个、多个或零个 Core 任务 ID；客户端组合任务不要求存在同名 Core 接口。
5. `AsstProxy` 记录 WPF 任务分类与 Core 任务 ID，全部追加完成后统一启动。

修改现有任务参数时，应保持“持久化配置 → 设置 ViewModel → Core 请求模型 → Core `set_params`”的单向转换，不建立双写或从 Core 参数反向补全配置。

## 两类任务类型

- `AsstTaskType` 是发送给 Core 的公共任务类型字符串，必须与 `Assistant::append_task` 接受的名称一致。
- `AsstProxy.TaskType` 是 WPF 内部的页面、日志和运行状态分类，可以表示 `RecruitCalc`、`Gacha`、`DepotMaintain` 等客户端概念。

二者不能混用。一个 WPF 类型可以映射到多个 Core 任务，例如库存保持映射到 `Depot` 和若干 `Fight`；多个页面也可以复用同一个 Core 类型，必须用任务 ID 区分请求。

## 回调路由

```text
MaaCore callback
  -> AsstProxy 原生回调入口
  -> taskchain + taskid + what + details
  -> WPF 任务分类与页面解析方法
  -> UI Dispatcher
  -> TaskItem、日志或页面结果
```

- 先用 `taskid` 和 `taskchain` 找到请求，再解释稳定的 `what` 事件与 `details`。
- 页面不能只按任务类型接收结果，否则旧请求或另一页面的同类任务可能污染当前状态。
- Core 回调来自后台线程；ViewModel 属性和可观察集合必须通过现有 Dispatcher 或 Stylet 的 `Execute.OnUIThread` 更新。
- 输入变化后，应让依赖旧输入的预检或识别结果失效。
- 调用方应容忍未知回调字段，但不能猜测已知字段的新类型或语义。
- 用户可见文本由 WPF 本地化，Core 事件名和原因码不直接作为最终文案。

## 启动、资源和更新生命周期

普通启动、资源热更新和应用版本更新是三条独立路径：

| 生命周期            | 首要入口                                            | 关注点                                                     |
| ------------------- | --------------------------------------------------- | ---------------------------------------------------------- |
| 普通启动与退出      | `Main/Bootstrapper.cs`                              | 日志、单实例、IoC、窗口、Core 句柄和退出清理               |
| Core 资源加载与重载 | `Main/AsstProxy.cs`                                 | 主资源、缓存、服务器与平台差异的加载顺序；仅在安全状态重载 |
| 资源包下载与导入    | `Models/ResourceUpdater.cs`                         | 下载、校验、缓存、导入以及空闲后重载                       |
| 应用版本更新        | `VersionUpdateSettingsUserControlModel`、更新对话框 | 下载应用包、退出当前进程并交给更新组件                     |

不要把资源重载当成应用更新，也不要在任务运行中直接替换 Core 正在使用的资源。页面只选择资源上下文，不按客户端名称复制 Core 或资源层已经表达的能力判断。

## 扩展检查

新增持久化队列项时检查：

1. `BaseTask` 派生类型、默认值和配置迁移。
2. 设置 UserControl、TaskSettings ViewModel 与配置切换刷新。
3. `AsstBaseTask` 或已有 Core 请求的客户端组合。
4. 一个或多个任务 ID 的保存、回调归属、失败恢复和停止。
5. 本地化和用户可见说明。

新增即时工具时检查：

1. 是否可复用已有 Core 类型或 `CustomTask`。
2. `RunningState`、连接、追加、启动和停止失败路径。
3. `AsstProxy` 回调分派、后台线程和结果失效条件。
4. 非平凡工具是否应拆出独立子 ViewModel、UserControl 和 Service，而不是继续扩大 `ToolboxViewModel`。
