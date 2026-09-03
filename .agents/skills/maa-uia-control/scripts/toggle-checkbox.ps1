param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][ValidateSet('On', 'Off')][string]$Desired
)
. "$PSScriptRoot/lib.ps1"

$win = Get-MaaWindow
Assert-NoDialog $win

$cb = Find-ByName $win $Name ([System.Windows.Automation.ControlType]::CheckBox)
if (-not $cb) { throw "checkbox '$Name' not found" }
if (-not $cb.Current.IsEnabled) { throw "checkbox '$Name' is disabled -- select an operator first?" }

$toggle = $cb.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
$before = $toggle.Current.ToggleState.ToString()
if ($before -ne $Desired) {
    $toggle.Toggle()
    Start-Sleep -Milliseconds 400
}
$after = $cb.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState.ToString()

$out = @("checkbox '$Name' before=$before after=$after (wanted $Desired)")
if ($after -ne $Desired) { $out += "WARNING: toggle did NOT take" }
Report 'toggle-checkbox' $out
