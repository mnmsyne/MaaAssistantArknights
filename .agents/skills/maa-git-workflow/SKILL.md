---
name: maa-git-workflow
description: 管理 MAA 个人 Codex Git 分支：同步基线、rebase 和恢复 codex/dev，隔离 wip 与 feat PR，并安全审查提交和推送范围。
---

# MAA 个人 Git 工作流

## 不变量

- 日常开发把最新 `upstream/dev-v2` 视为唯一基线；用户明确指定 Release、tag 或 commit 时，允许经过验证的“指定基线维护”模式。
- 只在 `codex/dev` 保存个人 AI 配置、`AGENTS.md`、`CLAUDE.md`、skills 与知识文档；禁止将其 merge、rebase 或完整 cherry-pick 到 PR 分支。
- 从干净的 `codex/dev` 手动创建 `wip/<topic>`，只在 WIP 分支提交产品改动。
- `wip/<topic>` 默认保留在本地；仅在用户明确授权并完成推送前检查后，才可推送到个人仓库 `origin` 的同名分支，禁止推送到官方主仓库 `upstream`。
- 从最新 `upstream/dev-v2` 创建 `feat/<topic>`，只 cherry-pick 审查过的产品提交。
- 禁止在 `codex/dev` merge 上游或执行可能产生 merge commit 的 `git pull`；常规模式只使用普通 `rebase upstream/dev-v2`，指定基线模式只按详细文档验证并重放个人提交。

## 授权与检查边界

- 只读检查可直接执行。暂存、提交、推送、改写历史、删除分支或覆盖远端前，先说明精确范围并确认用户已授权对应操作。
- 运行 `git status --short --branch` 并读取根目录与目标目录的 `AGENTS.md`。
- 工作区不干净时保留用户改动，不自动 stash、reset、clean、删除文件或切换分支。
- 子模块目录和父仓库 gitlink 默认只读；除非用户明确要求，不编辑子模块、不切换其提交，也不把 gitlink 变化纳入暂存或提交。
- 删除带改动的 WIP 前明确确认“保留”或“丢弃”。需要保留时使用带说明且包含未跟踪文件的 stash 并验证；需要丢弃时也先建立可核对的 stash，再仅在获得明确授权后删除该 stash。
- 不使用 `git add .`、`git add -A`、`--no-verify` 或无授权的强制推送。
- 审查疑似敏感信息时只报告规则和仓库相对路径，不向用户回显命中内容。
- 需要具体命令、误操作恢复或推送检查时，读取仓库根目录的 `.codex/docs/git-workflow.md`。

## 使用顺序

1. 先检查分支和工作区，再读取作用域内最近的 `AGENTS.md`。
2. 只读审查可以直接进行；任何 Git 写操作前先确认用户已授权精确动作和范围。
3. 执行同步、rebase、恢复、WIP 删除、提交、推送或 PR 隔离前，读取 `.codex/docs/git-workflow.md` 的对应章节，并以其中命令为唯一来源。
4. 操作后复核工作区、提交范围和目标引用；失败时报告原因，不绕过 hook 或扩大范围。

## 暂存与提交检查表

- 只暂存明确路径，不使用 `git add .` 或 `git add -A`。
- 提交前检查 staged 文件列表、完整 diff、`diff --check` 和疑似敏感信息。
- 提交前先总结工作区全部修改，按可独立审查的修改主题拆分提交；每个提交只包含一个连贯意图，不把无关修改一次性全部纳入同一提交。
- 提交摘要建议使用 `<type>(<scope>): <description>`。默认省略 `<scope>`、仅保留摘要且不写正文，即 `<type>: <description>`；只有用户明确要求时才添加 `<scope>` 或正文中的具体说明。
- commit message 正文不要按固定列宽手动强制换行；只在自然段、列表项或语义边界处换行。
- `codex/dev` 只提交个人配置与知识文档；`wip/*` 和 `feat/*` 只提交产品代码、必要测试与预期上游文档。
- 暂存和提交是不同授权；除非用户已明确同时授权，不因获得暂存授权自动提交。

## 分支路由

- 镜像：`origin/dev-v2` 只通过 GitHub 网页 **Sync fork** 更新；本地流程不负责推送该镜像。
- `codex/dev`：常规模式从干净工作区普通 rebase 到 `upstream/dev-v2`；指定基线模式先验证目标对象、tag 和祖先关系，再按详细文档重放个人提交。禁止 merge、`git pull` merge 和 `--rebase-merges`。改写已推送历史前必须记录精确远端 SHA，并获得 force-with-lease 授权。
- `wip/*`：从干净的 `codex/dev` 创建，开发期间不直接同步上游。用户明确要求时，可以按详细文档把 WIP 重接到更新后的 `codex/dev`；只重放产品提交并用 `range-diff` 验证。默认只保留本地；推送、改写远端历史或删除分支都需要单独授权。
- `feat/*`：从最新 `upstream/dev-v2` 创建，只 cherry-pick 已审查的产品提交；不得包含个人配置路径。
- WIP 删除：先处理未提交改动，再验证提交已在目标分支存在等价补丁；只有在补丁已保留或用户明确授权丢弃精确提交后，才可强制删除精确分支名。

## 创建或接管工作任务

确认当前位于 `codex/dev` 且工作区干净，再手动创建 `wip/<topic>`。WIP 只包含产品代码和必要测试；个人配置必须回到干净的 `codex/dev` 单独处理。

## 准备干净 PR 分支

1. 确认 WIP 分支无未提交修改。
2. 更新 `upstream/dev-v2`。
3. 列出 `codex/dev..wip/<topic>` 的提交并逐个审查文件范围。
4. 从上游基线创建 `feat/<topic>`。
5. 只 cherry-pick 产品提交，不迁移配置提交。

如果某个提交混合个人配置与产品代码，不要直接 cherry-pick。先回到 WIP 分支拆分提交，或在新分支仅恢复明确的产品路径并重新提交；不要用宽泛的排除 glob 猜测哪些文件安全。

## 推送前校验

- 检查工作区干净，并分别审查相对基线与相对远端的完整提交和文件范围。
- 上游已有告警必须定位来源；只确认个人 diff 干净，不静默忽略个人修改中的问题。
- `codex/dev` 改写远端历史时记录最后一次 fetch 得到的完整 SHA，并使用精确 lease；推送后再次 fetch 并比较完整 SHA。
- `wip/*` 只在用户明确授权后推送到 `origin` 的同名分支，不得推送到 `upstream`。
- `feat/*` 必须基于 `upstream/dev-v2`，并拒绝 `AGENTS.md`、`CLAUDE.md`、`.agents/**`、`.claude/**`、`.codex/**` 等个人路径进入 PR。
- 上游文档只有在与产品功能直接相关且用户希望纳入 PR 时才保留；PR base 使用上游 `dev-v2`。
