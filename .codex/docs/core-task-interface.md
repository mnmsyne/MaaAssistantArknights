# MaaCore 任务接口

本文只说明公共 C API、`AsstAppendTask` 任务类型、参数和回调契约，以及何时新增 Core 接口。功能对应的界面和资源位置见 [功能与界面定位](feature-locator.md)，Core 目录职责见 [仓库与架构定位](architecture.md)。

## 公共 C API

`include/AsstCaller.h` 定义稳定 ABI，`src/MaaCore/AsstCaller.cpp` 将句柄调用转发给 `Assistant`。

| 类别           | 主要能力                                                      |
| -------------- | ------------------------------------------------------------- |
| 资源与全局选项 | 设置用户目录、加载资源和进程级静态选项                        |
| 实例生命周期   | 创建、销毁 Assistant 句柄和获取版本                           |
| 连接           | 异步建立 ADB 连接、查询状态和返回主页；`AsstConnect` 已废弃   |
| 设备绑定与直控 | 异步绑定 Win32 窗口、连接附加参数、异步点击和强制截图         |
| 任务队列       | 追加任务、更新未结束任务的参数                                |
| 执行控制       | 启动、停止和查询运行状态                                      |
| 诊断           | 获取图像、UUID 和任务列表等辅助信息；`AsstLog` 只负责写入日志 |

Win32 绑定相关类型与函数（截图/输入方式枚举、`AsstAttachWindow`、`AsstAsyncAttachWindow`）仅在 Windows 平台导出；`AsstAttachWindow` 与 `AsstConnect` 已标注 deprecated，新调用方使用 `AsstAsyncConnect` 与 `AsstAsyncAttachWindow`。`include/AsstCallerExtra.h` 提供受 `ASST_WITH_EXTRA_CALLERS` 编译开关保护的实验性接口，仅供 MaaMacGui 等特定调用方使用，不属于稳定 ABI。

只有 ABI 无法表达新能力时才新增公共函数。修改已有签名、类型或生命周期语义时，必须同步 WPF `MaaService`、所有语言绑定和协议文档。

## 任务注册与执行

`Assistant::append_task` 根据任务类型字符串创建 `Task/Interface/` 下的 `InterfaceTask`，调用 `set_params()` 后加入队列。返回的任务 ID 是请求身份，任务按入队顺序执行，并通过 `AsstMsg` 回调报告状态和结果。

| 类别       | 代表任务                                                               | 典型用途                                 |
| ---------- | ---------------------------------------------------------------------- | ---------------------------------------- |
| 日常自动化 | `StartUp`、`Fight`、`Infrast`、`Recruit`、`Mall`、`Award`、`CloseDown` | 长期队列或直接调用                       |
| 作业与战斗 | `Copilot`、`ParadoxCopilot`、`SSSCopilot`、`SingleStep`                | 作业解析、编队和战斗状态机               |
| 长期玩法   | `Roguelike`、`Reclamation`                                             | 独立领域任务与插件                       |
| 识别工具   | `Depot`、`OperBox`、`VideoRecognition`                                 | 返回结构化识别结果                       |
| 资源化扩展 | `Custom`                                                               | 执行指定资源任务图，承载短期或单用途流程 |
| 内部诊断   | `Debug`                                                                | 开发和受控诊断                           |

任务类型字符串是跨语言契约。新增字符串通常不要求扩展 C API，但必须注册到 `Assistant::append_task`，并同步所有需要调用它的参数模型和绑定。

## 任务实现类型

| 类型                 | 使用场景                                             |
| -------------------- | ---------------------------------------------------- |
| `InterfaceTask`      | 公共任务的 `set_params` 边界和顶层执行包             |
| `PackageTask`        | 顺序组合已有子任务，并传播任务 ID、重试和退出状态    |
| `ProcessTask`        | 执行 `TaskData` 中的识别、动作、延迟、跳转和次数限制 |
| `AbstractTaskPlugin` | 在稳定任务节点验证、补充或拦截行为                   |
| 领域任务             | 战斗、基建、肉鸽等无法由资源图清晰表达的持续状态机   |
| `Vision` 分析器      | 从截图返回画面事实，不驱动设备                       |
| `Controller`         | 提供连接、截图、点击、滑动和输入等设备 I/O           |

所有任务继承的 `AbstractTask` 提供重试、启停、任务 ID、回调、插件和退出检查。不要在 `Assistant` 堆积具体业务流程，也不要从 Vision 分析器直接操作设备。

## 参数契约

- 参数使用稳定的英文键和值；本地化文本只存在于调用端。
- 传递干员、物品等实体时优先使用稳定 ID，不用本地化名称作唯一标识。
- `set_params()` 在入队前检查字段存在性、类型、范围和互斥组合；无效参数返回 `false`。
- 消耗资源或产生不可逆结果的行为必须显式表达意图，不能用猜测性默认值继续执行。
- `AsstSetTaskParams` 更新已有任务；实现必须明确哪些字段可在运行前或运行中安全修改。
- 长循环、翻页和等待阶段检查 `need_exit()`，确保停止请求及时生效。
- 最终设备操作前重新取得必要截图和识别结果；无法可靠识别时停止并报告原因。

资源任务名、模板名、回调事件名和 JSON 字段同样属于字符串契约。重命名时搜索 Core、WPF、语言绑定、主资源和所有差异覆盖。

## 回调契约

任务链状态使用 `AsstMsg`，细分进度和结构化结果通常通过 `SubTaskExtraInfo`：

```json
{
  "taskchain": "TaskType",
  "taskid": 1,
  "what": "StableEventName",
  "details": {}
}
```

| 字段        | 含义                                       |
| ----------- | ------------------------------------------ |
| `taskchain` | 所属公共任务类型                           |
| `taskid`    | 具体请求身份，用于隔离同类型任务和晚到回调 |
| `what`      | 稳定、非本地化的事件名                     |
| `details`   | 类型稳定的状态、原因码和结构化结果         |

新增结果优先扩展 `details`，调用方应容忍未知字段。不要为单一页面的显示需求扩展 C API，也不要改变已有字段类型。预期业务终止、识别失败、控制器失败和内部错误应可区分，使调用方能够恢复正确状态。

## 选择扩展方式

1. 只改页面模板、OCR 或跳转：修改 `resource/tasks/`、模板或识别配置。
2. 只在稳定节点增加判断：新增或复用 `AbstractTaskPlugin`。
3. 组合多个 Core 步骤：Core 内使用 `PackageTask`；仅单一客户端需要时优先客户端编排。
4. 资源图不足以表达复杂状态：新增领域任务，复用 Vision 和 Controller。
5. 多个调用方需要新的原子能力：新增 `Task/Interface/` 类型。
6. 现有句柄和任务 API 无法承载能力：最后才扩展 `AsstCaller` ABI。

短期活动和单用途流程优先使用资源任务与 `Custom`，不要为每个流程固化新的公共类型。

## 新增或修改接口任务

依次检查：

1. `Task/Interface/` 的任务类、稳定 `TaskType` 和 `set_params()`。
2. `Assistant::append_task` 注册与任务 ID 生命周期。
3. `PackageTask`、`ProcessTask`、插件或领域任务的复用边界。
4. 资源任务、模板、OCR、模型和服务器、平台覆盖。
5. 回调 `taskchain`、`what`、`details` 以及停止、重试和失败路径。
6. WPF `AsstTaskType`、`Models/AsstTasks/`、`AsstProxy` 和其他语言绑定。
7. 公共参数与回调文档的兼容性。
