# 个人 Codex 分支与 PR 隔离工作流

本文只保存需要精确执行的 Git 流程。分支职责、授权边界和通用修改规则以根目录 `AGENTS.md` 与 `$maa-git-workflow` 为准。

## 分支角色

- `upstream/dev-v2`：日常开发的唯一基线。
- `origin/dev-v2`：只通过 GitHub 网页 **Sync fork** 与 upstream 同步，本地不向它 push。
- `codex/dev`：个人 AI 配置和知识文档分支，不进入上游 PR。
- `wip/<topic>`：基于 `codex/dev` 的产品开发分支，默认仅保留本地。
- `feat/<topic>`：基于最新 `upstream/dev-v2`、用于上游 PR 的干净产品分支。

任何暂存、提交、推送、rebase、reset、stash、删除分支或历史改写都需要对应授权。工作区不干净时不得自动 stash、reset、clean、删除文件或切换分支。

## 更新 `codex/dev`

更新前确认当前分支和工作区，并检查是否存在活动 WIP。若活动 WIP 将在之后重接新基线，先按“重接现有 WIP”记录旧基线。

### 常规模式

常规更新只 rebase 到最新 upstream：

```powershell
git fetch upstream dev-v2
$targetBase = git rev-parse upstream/dev-v2
git switch codex/dev
git rebase $targetBase
```

### 指定基线模式

仅在用户明确指定 Release、tag 或 commit 时使用。目标必须是当前个人配置基线之后、最新 `upstream/dev-v2` 之内的 commit；同时给出 tag 与 SHA 时，两者必须解析到同一提交。

```powershell
git fetch --tags upstream dev-v2
$targetBase = git rev-parse "<target>^{commit}"
git merge-base --is-ancestor $targetBase upstream/dev-v2
$oldCodexSha = git rev-parse codex/dev
$oldBase = git merge-base codex/dev upstream/dev-v2
git merge-base --is-ancestor $oldBase $targetBase
git rebase --onto $targetBase $oldBase codex/dev
git range-diff "$oldBase..$oldCodexSha" "$targetBase..codex/dev"
```

任一祖先检查失败或 `range-diff` 不等价都立即停止。禁止 merge、`git pull` merge 和 `git rebase --rebase-merges`。

### 更新后检查

```powershell
git merge-base --is-ancestor $targetBase codex/dev
git rev-list --merges "$targetBase..codex/dev"
git diff --check "$targetBase...codex/dev"
git diff --name-status "$targetBase...codex/dev"
```

merge commit 检查应无输出。父仓库 gitlink 变化不属于个人修改；只有在对应子模块内部干净时，才同步目标基线记录的子模块提交。

用户明确要求把个人提交展开到工作区审查时，在补丁等价和文件范围检查完成后执行：

```powershell
$rebasedCodexSha = git rev-parse codex/dev
git diff --name-status "$targetBase..$rebasedCodexSha"
git reset --mixed $targetBase
git diff --cached --name-status
git status --short --branch --untracked-files=all
```

暂存区必须为空，工作区范围必须与展开前的个人提交一致。重新审查并提交前不得推送。

### 更新远端 `codex/dev`

rebase 或重新提交会改写历史。推送前比较个人补丁，并在获得精确推送授权后使用最后一次 fetch 得到的 SHA 作为 lease：

```powershell
git fetch origin codex/dev
$expectedOriginSha = git rev-parse origin/codex/dev
$remoteBase = git merge-base origin/codex/dev $targetBase
git range-diff "$remoteBase..origin/codex/dev" "$targetBase..codex/dev"
git diff --check "$targetBase...codex/dev"
git diff --name-status "$targetBase...codex/dev"
git push --force-with-lease="refs/heads/codex/dev:$expectedOriginSha" origin codex/dev:codex/dev
git fetch origin codex/dev
```

推送后本地与 `origin/codex/dev` 必须是同一完整 SHA。不得推送到 `upstream`。

## WIP 分支

从干净且已更新的 `codex/dev` 创建 WIP。WIP 只提交产品代码、必要测试和预期进入 PR 的上游文档，不包含个人 AI 配置。

```powershell
git switch codex/dev
git switch -c wip/<topic>
```

WIP 开发期间默认不直接同步 upstream。用户明确要求时，可以把现有 WIP 重接到更新后的 `codex/dev`。

普通 WIP 完成范围检查并获得首次推送授权后，只推送到个人仓库：

```powershell
git push -u origin wip/<topic>
```

### 重接现有 WIP

移动 `codex/dev` 前记录 WIP 的旧基线和头提交；已推送的 WIP 还要记录远端完整 SHA，并将这些值保留到重接完成：

```powershell
$oldWipBase = git merge-base --fork-point codex/dev wip/<topic>
$oldWipSha = git rev-parse wip/<topic>
git log --oneline "$oldWipBase..$oldWipSha"
git fetch origin wip/<topic>
$expectedOriginWipSha = git rev-parse origin/wip/<topic>
```

未推送的 WIP 跳过 fetch 和远端 SHA。无法可靠确定 `$oldWipBase` 时停止，不根据移动后的 merge-base 猜测。

更新并提交 `codex/dev` 后，从干净工作区只重放旧 WIP 基线之后的产品提交：

```powershell
git switch wip/<topic>
git rebase --onto codex/dev $oldWipBase wip/<topic>
git range-diff "$oldWipBase..$oldWipSha" "codex/dev..wip/<topic>"
git merge-base --is-ancestor codex/dev wip/<topic>
git diff --check "codex/dev...wip/<topic>"
git diff --name-status "codex/dev...wip/<topic>"
```

`range-diff` 必须等价，文件列表不得包含个人 AI 配置。已推送 WIP 还需要单独的历史改写授权；推送前再次 fetch，确认远端仍等于先前记录的 SHA，再使用精确 lease：

```powershell
git fetch origin wip/<topic>
if ((git rev-parse origin/wip/<topic>) -ne $expectedOriginWipSha) {
    throw '远端 WIP 已变化，停止推送'
}
git push --force-with-lease="refs/heads/wip/<topic>:$expectedOriginWipSha" origin wip/<topic>:wip/<topic>
git fetch origin wip/<topic>
```

推送后本地与远端 WIP 必须是同一完整 SHA。WIP 不得推送到 `upstream`。

### 删除 WIP

删除前检查工作区、未跟踪文件和独有提交。未提交修改必须由用户明确选择保留或丢弃；需要保留时使用包含未跟踪文件且名称明确的 stash。只有补丁已保存在目标分支，或用户明确授权丢弃精确提交后，才能删除精确 WIP 分支；删除 stash 需要单独授权。

## 暂存与提交

只暂存明确路径，不使用 `git add .` 或 `git add -A`。暂存和提交是不同授权。

```powershell
git status --short --branch
git add -- <path-1> <path-2>
git diff --cached --name-status
git diff --cached --check
git diff --cached
```

提交前总结全部工作区修改，并按可独立审查的意图拆分。检查令牌、密钥、凭据、私有远端、用户名、盘符和本机绝对路径；疑似敏感内容只报告规则与仓库相对路径，不回显命中内容。

默认提交摘要为 `<type>: <description>`，省略 scope、正文和手动换行。仅在用户明确要求时添加 scope 或正文。commit message 正文不要按固定列宽手动强制换行；每个自然段或列表项保持一行，只在语义边界换行。

提交失败时保留现场，不使用 `--no-verify`。

## 准备 PR 分支

确认 WIP 干净并可靠确定其创建基线；无法确定 fork point 时停止，不把当前 `codex/dev..wip/<topic>` 猜作产品范围。审查要迁移的产品提交后，从最新 upstream 创建 `feat/<topic>`，只 cherry-pick 选定提交：

```powershell
git fetch upstream dev-v2
$wipBase = git merge-base --fork-point codex/dev wip/<topic>
git log --oneline "$wipBase..wip/<topic>"
git switch -c feat/<topic> upstream/dev-v2
git cherry-pick <product-commit-1> <product-commit-2>
```

不得 merge 或 rebase `codex/dev`、`wip/*` 到 `feat/*`。检查完整 PR 范围：

```powershell
git merge-base --is-ancestor upstream/dev-v2 HEAD
git diff --check upstream/dev-v2...HEAD
git diff --name-status upstream/dev-v2...HEAD
git log --oneline upstream/dev-v2..HEAD
```

PR 中不得出现以下个人路径：

```text
AGENTS.md
**/AGENTS.md
CLAUDE.md
**/CLAUDE.md
.agents/**
.claude/**
.codex/**
```

确认分支干净、验证完成且获得推送授权后，只推送到个人仓库 `origin`：

```powershell
git push -u origin feat/<topic>
```

PR base 使用上游 `dev-v2`。
