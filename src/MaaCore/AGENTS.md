# MaaCore 修改约定

本文件只补充 `src/MaaCore/` 的局部规则，继承根目录 `AGENTS.md`。下文短路径相对此目录，`include/`、`resource/` 和 `.codex/` 等路径相对仓库根目录。

## 快速定位

| 位置                            | 职责                                            |
| ------------------------------- | ----------------------------------------------- |
| `AsstCaller.cpp`、`Assistant.*` | 公共 C API 落地、实例、连接、任务队列和回调队列 |
| `Task/Interface/`               | 公共任务类型、`set_params()` 和顶层任务包       |
| `Task/Miscellaneous/`           | 可复用的编队、战斗、公招和通用任务              |
| `Task/<domain>/`                | 作战、基建、肉鸽、生息演算和保全等领域状态机    |
| `Vision/`                       | 从截图提取事实的通用与领域分析器                |
| `Controller/`                   | 连接、截图、点击、滑动和输入的后端抽象          |
| `Config/TaskData.*`             | 资源任务图的合并、解析和生成                    |
| `Config/ResourceLoader.*`       | 任务、模板、OCR、模型和领域配置加载             |
| `Common/`、`Utils/`             | 日志、类型、JSON、文件和通用基础设施            |

公共任务契约见 `../../.codex/docs/core-task-interface.md`；从用户功能定位领域实现时使用 `../../.codex/docs/feature-locator.md`。

## 选择实现层

1. 页面识别、动作或跳转可由任务图表达时，修改 `resource/tasks/` 并复用 `ProcessTask`。
2. 只需在稳定节点增加判断或行为时，新增或复用 `AbstractTaskPlugin`。
3. 需要顺序组合已有能力时，使用 `PackageTask` 或 `Task/Miscellaneous/` 中的可复用任务。
4. 资源图不足以表达持续状态时，才在对应 `Task/<domain>/` 中实现领域任务。
5. 多个调用方需要新的原子能力时，才新增 `Task/Interface/` 类型；短期流程优先使用 `CustomTask`。
6. 只有现有句柄和任务 API 无法承载需求时，才扩展 `include/AsstCaller.h`。

## 接口与回调契约

- `Assistant::append_task` 是公共任务注册入口。新增任务类型时同步检查 `InterfaceTask::set_params()`、WPF `AsstTaskType` 与请求模型、其他语言绑定、资源和协议文档。
- `set_params()` 在入队前校验字段存在性、类型、范围和互斥组合；稳定英文键、任务类型、资源任务名和模板名不得无兼容方案改义。
- 回调使用 `taskchain`、`taskid`、稳定 `what` 和类型稳定的 `details`。新增信息优先扩展 `details`，不要为单一 UI 文案改变公共 C API。
- `taskid` 是请求身份；同类型任务和晚到回调必须可区分。预期业务终止、识别失败、控制器失败和内部错误应报告为可区分状态。
- `AsstSetTaskParams` 只更新已有任务；实现必须明确哪些字段能在运行前或运行中安全修改。
- 长循环、翻页和等待阶段检查 `need_exit()`；最终设备操作前重新取得必要截图或识别结果。

## 分层规则

- `Vision/` 只返回画面事实，不调用 Controller 或推进业务流程；优先复用 Matcher、OCRer、VisionHelper 和现有领域分析器。
- `Controller/` 只实现设备 I/O 和后端生命周期，保持连接、截图、触控、降级、重连和错误报告的后端抽象。
- 业务任务根据识别事实决定下一步并调用 Controller，不依赖具体 ADB、窗口或触控后端。
- `ResourceLoader` 与 `TaskData` 共同定义资源协议；修改字段、覆盖或能力标记时同步检查主资源、缓存、所有服务器和平台差异。
- 沿用相邻类的基类、工厂、插件、回调和错误处理模式，优先复用 `Common/`、`Utils/` 与 `Config/`，不在 `Assistant` 堆积业务流程。
- 保持公开 C API 向后兼容；必须修改 `include/` 时评估 WPF `MaaService`、所有语言绑定和外部调用方。

## 验证

- C++ 文件使用 `pre-commit run --files <changed-cpp-files>`，遵循根目录 `.clang-format`，不格式化无关文件。
- Windows 配置使用 `cmake --preset windows-x64`；定向构建使用 `cmake --build --preset windows-x64-Debug --target MaaCore`。
- 局部逻辑先做定向静态检查和构建；涉及 Controller、公共 API、资源契约或平台条件时扩大到相应调用方和平台。
- 检查停止、重试、连接失败、参数拒绝和晚到回调路径；需要运行测试或设备现场验证时遵循根目录授权规则。
