Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$ErrorActionPreference = 'Stop'
# Element names read from the live UI are Chinese. Without this, Windows PowerShell 5.1 writes stdout
# through the console's ANSI codepage and every name comes back as mojibake.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$global:AE = [System.Windows.Automation.AutomationElement]
$global:TS = [System.Windows.Automation.TreeScope]
$global:CT = [System.Windows.Automation.ControlType]

# --- labels -------------------------------------------------------------------------------------

# Read with an explicit UTF-8 decoder rather than Get-Content, whose default in PS 5.1 is the ANSI
# codepage and would mangle the Chinese labels.
function Get-Labels {
    if ($global:MaaLabels) { return $global:MaaLabels }
    $path = Join-Path $PSScriptRoot 'labels.json'
    $json = [System.IO.File]::ReadAllText($path, (New-Object System.Text.UTF8Encoding($false)))
    $global:MaaLabels = $json | ConvertFrom-Json
    return $global:MaaLabels
}

# --- element lookup -----------------------------------------------------------------------------

function Get-MaaWindow {
    $procs = Get-Process -Name 'MAA' -ErrorAction SilentlyContinue
    if (-not $procs) { throw 'MAA process not found (start MAA first; do not launch it unasked)' }
    if (@($procs).Count -gt 1) {
        $ids = ($procs | ForEach-Object { $_.Id }) -join ', '
        throw "multiple MAA processes found (pids: $ids) -- close the extra instance(s) first, this skill drives an arbitrary one"
    }
    $proc = $procs
    $cond = New-Object System.Windows.Automation.PropertyCondition($global:AE::ProcessIdProperty, $proc.Id)
    $win = $global:AE::RootElement.FindFirst($global:TS::Children, $cond)
    if (-not $win) { throw 'MAA window not found in UIA tree' }
    return $win
}

function Find-ByName($root, $name, $type) {
    $conds = New-Object System.Collections.ArrayList
    [void]$conds.Add((New-Object System.Windows.Automation.PropertyCondition($global:AE::NameProperty, $name)))
    if ($type) {
        [void]$conds.Add((New-Object System.Windows.Automation.PropertyCondition($global:AE::ControlTypeProperty, $type)))
    }
    $cond = if ($conds.Count -eq 1) { $conds[0] } else {
        New-Object System.Windows.Automation.AndCondition([System.Windows.Automation.Condition[]]$conds.ToArray())
    }
    return $root.FindFirst($global:TS::Descendants, $cond)
}

function Find-ByAid($root, $aid) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($global:AE::AutomationIdProperty, $aid)
    return $root.FindAll($global:TS::Descendants, $cond)
}

function Find-AllByType($root, $type) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($global:AE::ControlTypeProperty, $type)
    return $root.FindAll($global:TS::Descendants, $cond)
}

# --- modal dialogs ------------------------------------------------------------------------------

# HandyControl's MessageBox (MessageBoxHelper.Show) renders as a Window element NESTED INSIDE the main
# window's UIA subtree, not as a separate top-level OS window. Scanning RootElement.Children -- the
# obvious thing -- reports "1 window" and misses it entirely. That blindness is what let four stacked
# confirmation dialogs accumulate unnoticed during live testing. Always scan descendants.
function Get-MaaDialogs($win) {
    if (-not $win) { $win = Get-MaaWindow }
    $found = New-Object System.Collections.ArrayList
    foreach ($w in Find-AllByType $win $global:CT::Window) {
        if (-not $w.Current.IsOffscreen) { [void]$found.Add($w) }
    }
    return $found
}

# HandyControl's MessageBox does NOT expose its message as a ControlType.Text element -- that only ever
# yields the button captions (confirmed by dumping a live 'Yes'/No' dialog: the only Text nodes found were
# nested inside the buttons themselves). The actual message renders as a read-only selectable text control,
# which UIA surfaces as Edit or Document with the content in ValuePattern or TextPattern, not Name. Scan
# those too, and explicitly exclude Text nodes that are descendants of a Button so button captions don't
# leak into the body.
function Format-Dialog($dlg) {
    $btns = @()
    foreach ($b in Find-AllByType $dlg $global:CT::Button) {
        $btns += "$($b.Current.AutomationId)='$($b.Current.Name)'"
    }

    $isInsideButton = {
        param($el)
        $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
        $parent = $walker.GetParent($el)
        while ($parent) {
            if ($parent.Current.ControlType -eq $global:CT::Button) { return $true }
            if ($parent.Current.NativeWindowHandle -eq $dlg.Current.NativeWindowHandle -and $parent.Current.NativeWindowHandle -ne 0) { break }
            $parent = $walker.GetParent($parent)
        }
        return $false
    }

    $texts = @()
    foreach ($t in Find-AllByType $dlg $global:CT::Text) {
        if (& $isInsideButton $t) { continue }
        $s = $t.Current.Name
        if ($s -and $s.Length -gt 0) { $texts += $s }
    }
    foreach ($e in Find-AllByType $dlg $global:CT::Edit) {
        try {
            $val = $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
            if ($val -and $val.Length -gt 0) { $texts += $val }
        } catch { }
    }
    foreach ($d in Find-AllByType $dlg $global:CT::Document) {
        try {
            $range = $d.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange
            $val = $range.GetText(-1)
            if ($val -and $val.Length -gt 0) { $texts += $val }
        } catch { }
    }

    $body = ($texts -join ' / ')
    # Confirmation dialogs like OperDevelopConsumeConfirmation are multi-line and can run past 120 chars;
    # 400 comfortably covers those while still keeping the report readable.
    if ($body.Length -gt 400) { $body = $body.Substring(0, 400) + '...' }
    return "title='$($dlg.Current.Name)' body='$body' buttons=[$($btns -join ', ')]"
}

# Every action script calls this FIRST. A modal dialog swallows the interaction that follows, so
# acting while one is open both fails silently and leaves the app in a state the next call misreads.
function Assert-NoDialog($win) {
    $dlgs = Get-MaaDialogs $win
    if ($dlgs.Count -gt 0) {
        $lines = @("refusing to act: $($dlgs.Count) modal dialog(s) already open -- answer them with click-dialog.ps1 first")
        foreach ($d in $dlgs) { $lines += "  $(Format-Dialog $d)" }
        throw ($lines -join "`n")
    }
}

# --- patterns -----------------------------------------------------------------------------------

function Invoke-Element($el) {
    $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Select-Element($el) {
    $el.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
}

function Set-EditValue($el, $text) {
    $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($text)
}

# Invoke and then report what OBSERVABLY changed. The old click.ps1 wrote "invoked '<name>'" the
# instant Invoke() returned, which is true but worthless: Invoke() succeeds whether the click opened
# a dialog, started a task, or hit a no-op handler. Reporting the dialog delta and the button's new
# enabled state is what turns a click into evidence.
function Invoke-Action($win, $el, $label) {
    $before = (Get-MaaDialogs $win).Count
    Invoke-Element $el
    Start-Sleep -Milliseconds 900
    $dlgs = Get-MaaDialogs $win
    $lines = @("invoked '$label'")
    $lines += "  enabled after: $($el.Current.IsEnabled)"
    $lines += "  dialogs: $before -> $($dlgs.Count)"
    foreach ($d in $dlgs) { $lines += "  open dialog: $(Format-Dialog $d)" }
    if ($dlgs.Count -gt $before) {
        $lines += "  ACTION REQUIRED: this click opened a dialog and the action has NOT run yet."
        $lines += "  Answer it with: click-dialog.ps1 -Choice Yes|No   (do NOT click '$label' again)"
    }
    return $lines
}

# --- output -------------------------------------------------------------------------------------

function Write-Utf8($path, $lines) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($path, [string[]]$lines, $enc)
}

# Write to out/<name>.txt and echo to stdout, so a run is readable either way.
# Prepends a UTC timestamp so a stale out/<name>.txt from a previous run can never be mistaken for the
# current one -- the file's mtime alone doesn't survive being read back into a transcript.
function Report($name, $lines) {
    $stamped = @("[$([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))] $name") + $lines
    Write-Utf8 (Join-Path $PSScriptRoot "..\out\$name.txt") $stamped
    $stamped | ForEach-Object { Write-Output $_ }
}
