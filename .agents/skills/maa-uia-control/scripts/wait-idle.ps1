param(
    [int]$TimeoutSeconds = 300,
    [int]$PollMilliseconds = 3000,
    [int]$StablePolls = 3
)

. "$PSScriptRoot/lib.ps1"

# Neither the preflight nor the start button (labels.json: buttons.preflight / buttons.start) gates on
# a CanExecute -- there is no CanRunPreflight / CanStopTask in ToolboxViewModel.OperDevelop.cs, so all
# three buttons (including buttons.stop) report IsEnabled=true throughout a run. A first version of
# this script polled buttons.stop going disabled as the completion signal; live-tested, it never fired
# even minutes after the task chain had genuinely finished (confirmed via asst.log
# TaskChainCompleted), because the button state that signal depended on doesn't exist.
#
# What DOES change while work is happening is the OperDevelop log panel (LogCardViewModels): each subtask
# step appends at least one line. So "idle" here means "the log panel's line count stopped growing for
# $StablePolls consecutive polls" -- a generic signal that works whether the running operation is a
# preflight, an execute, or a continue, without needing per-button wiring.
#
# Same caveat as before: a dialog appearing (e.g. the material-consumption confirmation after pressing
# buttons.start) pauses the task chain -- this script does NOT auto-answer it, it surfaces the dialog
# and returns immediately so the caller can decide via click-dialog.ps1.

$L = Get-Labels
$out = New-Object System.Collections.ArrayList
$win = Get-MaaWindow

$tab = Find-ByName $win $L.operDevelopTab ([System.Windows.Automation.ControlType]::TabItem)
if (-not $tab) {
    throw "operdevelop tab not found -- run navigate.ps1 first"
}

function Get-LogLineCount($window) {
    $tabEl = Find-ByName $window $script:L.operDevelopTab ([System.Windows.Automation.ControlType]::TabItem)
    if (-not $tabEl) { return -1 }
    $texts = Find-AllByType $tabEl ([System.Windows.Automation.ControlType]::Text)
    return @($texts | Where-Object { $_.Current.Name -and $_.Current.Name.Trim().Length -gt 0 }).Count
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$polls = 0
$stableStreak = 0
$previousCount = -1
while ((Get-Date) -lt $deadline) {
    $polls++
    $win = Get-MaaWindow
    $dlgs = Get-MaaDialogs $win
    if ($dlgs.Count -gt 0) {
        [void]$out.Add("poll $polls : $($dlgs.Count) dialog(s) open -- answer with click-dialog.ps1 to let it continue")
        foreach ($d in $dlgs) { [void]$out.Add("  $(Format-Dialog $d)") }
        Report 'wait-idle' $out
        exit 0
    }

    $count = Get-LogLineCount $win
    if ($count -eq $previousCount) {
        $stableStreak++
    }
    else {
        $stableStreak = 0
    }
    $previousCount = $count

    if ($stableStreak -ge $StablePolls) {
        [void]$out.Add("idle after $polls poll(s), log line count stable at $count for $stableStreak polls")
        Report 'wait-idle' $out
        exit 0
    }
    Start-Sleep -Milliseconds $PollMilliseconds
}

[void]$out.Add("TIMEOUT after ${TimeoutSeconds}s and $polls poll(s) -- log panel is still growing (or stuck)")
Report 'wait-idle' $out
exit 1
