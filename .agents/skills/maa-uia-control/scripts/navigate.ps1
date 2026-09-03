. "$PSScriptRoot/lib.ps1"

# Idempotently puts MAA on toolbox > operdevelop (labels.json: toolboxTab / operDevelopTab) and
# VERIFIES each hop landed.
# Replaces three overlapping ad-hoc scripts (select-operdevelop-tab / select_operdevelop_tab /
# select_toolbox_tab), none of which checked whether the tab actually became selected -- so a failed
# hop looked identical to a successful one and every later script then operated on the wrong page.

$L = Get-Labels
$win = Get-MaaWindow
Assert-NoDialog $win
$out = @()

function Select-Tab($win, $names, $what) {
    foreach ($n in $names) {
        $tab = Find-ByName $win $n ([System.Windows.Automation.ControlType]::TabItem)
        if (-not $tab) { continue }
        $sel = $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        if ($sel.Current.IsSelected) { return @("$what : already on '$n'") }
        Select-Element $tab
        Start-Sleep -Milliseconds 900
        # Re-find: switching tabs rebuilds the subtree and can invalidate the cached element.
        $tab = Find-ByName $win $n ([System.Windows.Automation.ControlType]::TabItem)
        $now = $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected
        if (-not $now) { throw "$what : clicked '$n' but it did not become selected" }
        return @("$what : selected '$n'")
    }
    throw "$what : none of these tabs found: $($names -join ', ')"
}

# The toolbox tab's Name falls back to the ViewModel type when no DisplayName is set, which is what
# the live tree actually showed. Try the localized label first, then that.
$out += Select-Tab $win @($L.toolboxTab, $L.toolboxTabFallback) 'toolbox'
$out += Select-Tab $win @($L.operDevelopTab) 'operdevelop'

# Landing proof: the page's own controls must now be present and onscreen.
foreach ($n in @($L.buttons.preflight, $L.buttons.start)) {
    $b = Find-ByName $win $n ([System.Windows.Automation.ControlType]::Button)
    if ($b) {
        $out += "  button '$n' enabled=$($b.Current.IsEnabled) offscreen=$($b.Current.IsOffscreen)"
    } else {
        $out += "  button '$n' NOT FOUND -- navigation did not land on the expected page"
    }
}

Report 'navigate' $out
