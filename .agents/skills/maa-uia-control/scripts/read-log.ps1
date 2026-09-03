param(
    [int]$Last = 0
)

. "$PSScriptRoot/lib.ps1"

# The OperDevelop log panel (LogCardViewModels, ToolboxView.xaml ~line 642) is a plain ItemsControl inside
# an hc:ScrollViewer -- no explicit VirtualizingStackPanel override, so unlike a ListBox it should NOT
# UI-virtualize; every log line should exist in the UIA tree even when scrolled out of view (IsOffscreen).
# That is inferred from the XAML, not verified against a long-running list -- if MAA is ever refactored to
# a virtualizing panel here, only the currently-realized rows would be readable and this comment would be
# stale. Each row renders as a controls:TextBlock, which UIA reports as ControlType.Text with the rendered
# string as Name -- there is no AutomationId on the log panel itself, so rows are found by scanning all Text
# descendants under the operdevelop tab page (labels.json: operDevelopTab), in visual-tree order
# (top-to-bottom, oldest first).
#
# The bound Color (UiLogColor.*) is a WPF resource-brush key, not exposed via UIA at all -- this script can
# only assert on text content, never on whether a line rendered red/green/etc. Verify color by eye or by
# screenshot.

$L = Get-Labels
$out = New-Object System.Collections.ArrayList
$win = Get-MaaWindow

$tab = Find-ByName $win $L.operDevelopTab ([System.Windows.Automation.ControlType]::TabItem)
if (-not $tab) {
    throw "operdevelop tab not found -- run navigate.ps1 first"
}

# EstimatedTimeText / PreflightSummary are plain bound TextBlocks with no AutomationId either (ToolboxView.xaml
# :604 and :613); they cannot be reliably distinguished from the log body by type, so they are folded into the
# same ordered text dump below rather than pulled out separately -- both is more informative than only one.
$texts = Find-AllByType $tab ([System.Windows.Automation.ControlType]::Text)
foreach ($t in $texts) {
    $s = $t.Current.Name
    if ($s -and $s.Trim().Length -gt 0) {
        [void]$out.Add($s)
    }
}

if ($Last -gt 0 -and $out.Count -gt $Last) {
    $out = $out.GetRange($out.Count - $Last, $Last)
}

Report 'read-log' $out
