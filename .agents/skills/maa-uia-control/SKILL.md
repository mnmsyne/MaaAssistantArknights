---
name: maa-uia-control
description: 通过 Windows UI Automation（UIA）驱动正在运行的 MAA WPF 实例——点击按钮、勾选复选框、选择下拉项与页签、应答模态对话框、读取界面日志面板。这是对运行中的 MAA 做现场测试的唯一认可交互方式；禁止直接脚本化 MaaCore，禁止向模拟器注入 OS 级输入。用户要求操作或检查 MAA 界面、对 MAA 做现场回归验证、需要确认按钮或复选框实际状态时使用本 skill。
---

# maa-uia-control

Drives the MAA WPF app's own controls through `System.Windows.Automation` (UIA), the same
accessibility layer screen readers use. It clicks buttons, reads/sets checkbox and combo state,
answers modal dialogs, and selects operator-search results — nothing more. It never touches the
emulator window, never sends raw OS input (no `SendKeys`/mouse-hook/pixel-coordinate clicking), and
never calls into MaaCore directly.

This copy lives in `.agents/skills/` as part of the personal config branch (`codex/dev`). The
`.claude/skills/maa-uia-control` copy is a gitignored local twin for Claude Code sessions; keep
behavioral changes in both copies in sync.

## When to use this

Whenever a task requires observing or driving the live MAA WPF UI — verifying a toolbox checkbox
reflects a config change, confirming a button's enabled state before/after a fix, selecting an
operator during live-device regression testing.

## Safety / authorization (binding)

- **Requires explicit user authorization** before each live run, same as launching MAA or operating
  the emulator (per `AGENTS.md` / `CLAUDE.md`). Ask even if you already asked earlier in the session.
- Only drives MAA's own WPF window (located by `Get-Process -Name 'MAA'` + UIA). Never targets the
  emulator window or sends input elsewhere.
- Read-only inspection (`state.ps1`, `dialogs.ps1`, `dump-tree.ps1`, `dump-combos.ps1`,
  `inspect-combo.ps1`) is lower-risk than action scripts (`navigate.ps1`, `click.ps1`,
  `click-dialog.ps1`, `select-oper.ps1`, `select-combo.ps1`, `toggle-checkbox.ps1`).
- If MAA isn't running, ask the user before launching it — don't launch it to make a script succeed.

## The rule that matters most: one click, then look

**Never click the same button twice because "nothing happened."** MAA gates destructive actions
behind a `MessageBoxHelper.Show` confirmation. HandyControl renders that as a `Window` element
**nested inside the main window's UIA subtree** — not a separate top-level OS window. Scanning
`RootElement.Children` reports "1 window" and misses it completely.

In an earlier session that blindness cost ~15 minutes: `click.ps1 "确认并开始"` printed
`invoked '确认并开始'` four times in a row (truthfully — `Invoke()` did succeed each time), no task
ever started, and four stacked confirmation dialogs piled up unnoticed.

The scripts now make that failure mode impossible to miss:

- `Assert-NoDialog` runs at the top of every action script. Acting while a modal is open **throws**,
  listing the open dialogs, instead of silently no-opping.
- `Invoke-Action` reports the **dialog count delta** and the button's post-click enabled state. If a
  dialog opened it prints `ACTION REQUIRED ... do NOT click again`.
- `click-dialog.ps1` is the only way to answer one.

So the loop is always: `click.ps1 <button>` → read the output → if a dialog opened,
`click-dialog.ps1 -Choice Yes` → confirm via `dialogs.ps1` that the count is back to 0.

## Scripts

Invoked via the Bash tool with `powershell.exe`, run from the repo root. Every script echoes to
stdout **and** writes `out/<script-name>.txt`.

```bash
powershell.exe -NoProfile -File .agents/skills/maa-uia-control/scripts/state.ps1
```

Scope: `state.ps1`, `navigate.ps1`, `wait-idle.ps1`, `read-log.ps1` and `select-oper.ps1` target the
OperDevelop (干员培养) toolbox tab; `dialogs.ps1`, `dump-tree.ps1`, `dump-combos.ps1`,
`inspect-combo.ps1`, `click.ps1`, `click-dialog.ps1`, `select-combo.ps1` and `toggle-checkbox.ps1`
are generic across MAA's window.

### Inspect (read-only)

| script                       | purpose                                                                                                                                                                                                                                                                                                                     |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `state.ps1`                  | Snapshot: open dialogs, operdevelop tab selection, operator textbox value, the three skill checkboxes, and the preflight/start/stop buttons' enabled state. **Start here.**                                                                                                                                                 |
| `dialogs.ps1`                | List open modal dialogs with their body text and button AutomationIds. Run this the moment a click seems to do nothing.                                                                                                                                                                                                     |
| `dump-tree.ps1`              | Full UIA tree to depth 14 (type, name, AutomationId, offscreen). Use when you don't know an element's identity.                                                                                                                                                                                                             |
| `dump-combos.ps1`            | Just the ComboBox / ComboBoxItem / ListItem elements — gets you the `-Index` for `select-combo.ps1`.                                                                                                                                                                                                                        |
| `inspect-combo.ps1 -Index N` | One combo's patterns and children.                                                                                                                                                                                                                                                                                          |
| `read-log.ps1 [-Last N]`     | Dump the OperDevelop log panel's text content in visual-tree order (oldest first); `-Last N` trims to the tail. The only way to verify what `AddLog(...)` actually rendered — e.g. the multi-line 材料复核 output. Cannot see line **color** (a bound brush key, invisible to UIA) — verify red/green by eye or screenshot. |

### Act

| script                                                    | purpose                                                                                                                                                                                                                                                                                                                                                                                |
| --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `navigate.ps1`                                            | Idempotently select toolbox → operdevelop, verifying each hop actually landed.                                                                                                                                                                                                                                                                                                         |
| `click.ps1 <button-name>`                                 | Click a button by exact name; refuses if a dialog is open, disabled, or offscreen; reports the dialog delta afterwards.                                                                                                                                                                                                                                                                |
| `click-dialog.ps1 -Choice Yes\|No [-All]`                 | Answer a modal. `-All` drains every stacked dialog (pair it with `No` for cleanup; `-All Yes` would confirm each queued action separately).                                                                                                                                                                                                                                            |
| `select-oper.ps1 <operator-name>`                         | Type into the operator autocomplete, expand, select the exact match, then report skill-checkbox enablement as proof the binding took. Falls back to progressively shorter suffixes of the name for alter-egos (see trap 3 below).                                                                                                                                                      |
| `select-combo.ps1 -Index N -Contains <substring>`         | Select a combo item, then **verify** the selection actually changed.                                                                                                                                                                                                                                                                                                                   |
| `toggle-checkbox.ps1 -Name <label> -Desired On\|Off`      | Set a checkbox via TogglePattern and verify.                                                                                                                                                                                                                                                                                                                                           |
| `wait-idle.ps1 [-TimeoutSeconds N] [-PollMilliseconds N]` | Poll until the OperDevelop log panel's line count stops growing for several consecutive polls instead of a blind `Start-Sleep` — a preflight run alone can take several minutes. Button state is NOT a usable idle signal (no CanExecute gates, so 停止 stays enabled during a run). Reports open dialogs without answering them; answering is still your call via `click-dialog.ps1`. |

`lib.ps1` holds the shared helpers (`Get-MaaWindow`, `Find-ByName`, `Find-ByAid`, `Find-AllByType`,
`Get-MaaDialogs`, `Assert-NoDialog`, `Format-Dialog`, `Invoke-Element`, `Invoke-Action`,
`Select-Element`, `Set-EditValue`, `Get-Labels`, `Report`). Dot-sourced, never run directly.

## Traps that will bite you again

**1. Never put non-ASCII in a `.ps1`.** Windows PowerShell 5.1 reads a BOM-less `.ps1` through the
ANSI codepage. A Chinese literal then either silently matches nothing or blows up with
`TerminatorExpectedAtEndOfString` — and the error points nowhere near the real cause. This cost four
separate debugging detours in one session.

All UI text lives in `scripts/labels.json`, read through an explicit UTF-8 decoder via `Get-Labels`.
Every `.ps1` in this skill is pure ASCII, comments included — keep it that way. Changing MAA's UI
language is now a one-file edit. (Chinese passed as a _command-line argument_, e.g.
`select-oper.ps1 "予愿安洁莉娜"`, is fine — the hazard is only literals inside the file.)

**2. Combo items expose the bound object's `ToString()`, not the display text.** For an
`ItemsSource`-bound ComboBox the item is a `ListItem` (not `ComboBoxItem`) whose `Name` is something
like `TrainingModeOption { Display = ..., Value = efficiency }`. Match `-Contains` against a stable
fragment such as `= efficiency }`, never against the visible label. `select-combo.ps1` searches both
control types and verifies the resulting selection, but you still have to pick a substring that
exists.

**3. The operator search filters on an operator's _base_ name, not its alter-ego display name.** An
alter-ego's display name is `<alter-title><base-name>` (e.g. 予愿安洁莉娜 is the 予愿 alter of 安洁莉娜),
and the game's own search box does not match on the alter-title prefix — typing the full display name
returns zero candidates even though the operator exists. `select-oper.ps1` handles this automatically:
if the full name yields no dropdown match, it retries with the query's leading character stripped, one
character at a time, until a candidate's full name matches the _original_ target exactly (or the query
gets too short to be meaningful). You don't need to do anything differently — just pass the full display
name — but if you ever see `NO exact match` for a name you know exists, this is why.

**4. `Format-Dialog`'s body text comes from `Edit`/`Document` elements, not `Text`.** A HandyControl
`MessageBox`'s message is a read-only selectable text control, which UIA exposes as `ControlType.Edit`
or `Document` with the content in `ValuePattern`/`TextPattern` — **not** `Name`. The only `Text`
elements inside such a dialog are the button captions nested inside the buttons themselves. An earlier
version of `Format-Dialog` scanned only `Text` and reported `body='确认 / 取消'` for every dialog — that
was literally the button labels, not the message. `Format-Dialog` now scans `Edit`/`Document` too and
excludes `Text` nodes that are descendants of a `Button`, so `OperDevelopConsumeConfirmation` and
similar multi-line confirmation bodies are actually readable.

## Output

`out/` is scratch — safe to delete between runs, and kept out of git status via `.git/info/exclude`.
Read `out/<script>.txt` back with the Read tool. Every file starts with a
`[<UTC timestamp>] <script-name>` line, so a stale result from a previous run can't be mistaken for
the current one.
