param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Name
)
. "$PSScriptRoot/lib.ps1"

$win = Get-MaaWindow

# Refuse to click while a modal dialog is open. Clicking anyway is what stacked four confirmation
# dialogs during live testing: each Invoke() "succeeded", none of the actions ran, and the app looked
# unresponsive.
Assert-NoDialog $win

$b = Find-ByName $win $Name ([System.Windows.Automation.ControlType]::Button)
if (-not $b) { throw "button '$Name' not found" }
if (-not $b.Current.IsEnabled) { throw "button '$Name' is disabled" }
if ($b.Current.IsOffscreen) { throw "button '$Name' is offscreen -- navigate to its page first" }

Report 'click' (Invoke-Action $win $b $Name)
