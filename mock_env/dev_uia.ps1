param(
    [int]$ProcId = 0,
    [string]$List = "",
    [string]$ClickName = ""
)
# Windows PowerShell 5.1 + UIA 枚举/点击 Qt 应用按钮
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$proc = Get-Process -Id $ProcId -ErrorAction Stop
$root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
Write-Output ("root=" + $root.Current.Name + " class=" + $root.Current.ClassName)
if ($List) {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    $btns = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
    Write-Output ("buttons=" + $btns.Count)
    for ($i = 0; $i -lt $btns.Count; $i++) {
        $b = $btns.Item($i)
        $rect = $b.Current.BoundingRectangle
        Write-Output ("btn[$i] name=[" + $b.Current.Name + "] enabled=" + $b.Current.IsEnabled +
                      " x=" + [int]$rect.X + " y=" + [int]$rect.Y +
                      " w=" + [int]$rect.Width + " h=" + [int]$rect.Height)
    }
}
if ($List -eq "ALL") {
    $condAll = [System.Windows.Automation.Condition]::TrueCondition
    $els = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condAll)
    for ($i = 0; $i -lt [Math]::Min($els.Count, 400); $i++) {
        $e = $els.Item($i)
        $n = $e.Current.Name
        $ct = $e.Current.ControlType.ProgrammaticName
        if ($n -and $ct -match 'Text|Button|Edit|Combo') {
            $rect = $e.Current.BoundingRectangle
            Write-Output ("el[$i] $ct [" + $n + "] x=" + [int]$rect.X + " y=" + [int]$rect.Y)
        }
    }
}
if ($ClickName) {
    $condName = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $ClickName)
    $condBtn = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    $and = New-Object System.Windows.Automation.AndCondition($condName, $condBtn)
    $found = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $and)
    if ($found) {
        $inv = $found.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $inv.Invoke()
        Write-Output ("INVOKED name=[" + $ClickName + "]")
    } else {
        Write-Output ("NOT_FOUND name=[" + $ClickName + "]")
    }
}
