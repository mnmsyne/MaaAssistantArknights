param(
    [Parameter(Mandatory = $true)][ValidateSet('Yes', 'No')][string]$Choice,
    # Answer every stacked dialog, not just the first. Use with -Choice No to clean up after a
    # sequence of stray clicks; think twice before combining it with -Choice Yes, which would
    # confirm each queued action separately.
    [switch]$All
)
. "$PSScriptRoot/lib.ps1"

$win = Get-MaaWindow
$out = @()

# Dialogs are matched by ControlType.Window, not by title. Titles vary by call site and are
# locale-dependent; the earlier version hardcoded one Chinese title and found nothing otherwise.
$rounds = 0
while ($true) {
    $dlgs = Get-MaaDialogs $win
    if ($dlgs.Count -eq 0) {
        $out += if ($rounds -eq 0) { 'no dialog open -- nothing to answer' } else { 'all dialogs answered' }
        break
    }
    if ($rounds -gt 0 -and -not $All) { break }
    if ($rounds -ge 12) { $out += "stopping after $rounds rounds; $($dlgs.Count) still open"; break }

    $dlg = $dlgs[0]
    $out += "dialog: $(Format-Dialog $dlg)"
    $cond = New-Object System.Windows.Automation.PropertyCondition($global:AE::AutomationIdProperty, $Choice)
    $btn = $dlg.FindFirst($global:TS::Descendants, $cond)
    if (-not $btn) { throw "button with AutomationId '$Choice' not found in dialog: $(Format-Dialog $dlg)" }
    Invoke-Element $btn
    Start-Sleep -Milliseconds 900
    $rounds++
    $out += "  clicked $Choice (remaining: $((Get-MaaDialogs $win).Count))"
    if (-not $All) { break }
}

$out += "dialogs still open: $((Get-MaaDialogs $win).Count)"
Report 'click-dialog' $out
