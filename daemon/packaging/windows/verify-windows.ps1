# Writes a report of what a Windows machine is and what the daemon did on it,
# so results from different machines can be compared. Changes nothing.
#
#   powershell -ExecutionPolicy Bypass -File .\verify-windows.ps1 [-Report <path>]
#
# Run elevated to include the service configuration and device registry values.

[CmdletBinding()]
param(
    [string]$ServiceName = "esp32-ir-remote",
    [string]$VendorId    = "1209",
    [string]$ProductId   = "0001",

    # Defaults to the log directory.
    [string]$Report,

    [int]$LogLines = 60
)

$ErrorActionPreference = "Continue"

$logDir  = Join-Path $env:ProgramData "ESP32IRRemote"
$logFile = Join-Path $logDir "daemon.log"

if (-not $Report) {
    $stamp  = Get-Date -Format "yyyyMMdd-HHmmss"
    $Report = Join-Path $logDir "verify-$($env:COMPUTERNAME)-$stamp.txt"
}

$elevated = ([Security.Principal.WindowsPrincipal] `
             [Security.Principal.WindowsIdentity]::GetCurrent()
            ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$out = New-Object System.Collections.Generic.List[string]
function Say([string]$line = "") { $out.Add($line); Write-Host $line }

Say "ESP32 IR Remote — Windows verification report"
Say ("Generated  : " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Say ("Machine    : $env:COMPUTERNAME")
Say ("Elevated   : $elevated")
if (-not $elevated) {
    Say "             (service config and device registry values need elevation)"
}
Say ("Windows    : " + (Get-CimInstance Win32_OperatingSystem).Caption +
     " build " + [System.Environment]::OSVersion.Version.Build)
Say ""

# --- 1. What kind of machine is this ----------------------------------------
#
# The sleep model explains which events to expect: no PBT_APMSUSPEND is normal
# on a Modern Standby laptop and a defect on an S3 desktop.
Say "== Power model =="
Say ""
Say "powercfg /a:"
try {
    powercfg /a 2>&1 | ForEach-Object { Say ("  " + $_) }
} catch {
    Say "  powercfg unavailable: $_"
}
Say ""

# Fast Startup is the other axis, and it is not in `powercfg /a` output.
$hiberboot = $null
try {
    $hiberboot = (Get-ItemProperty `
        -Path "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power" `
        -Name "HiberbootEnabled" -ErrorAction Stop).HiberbootEnabled
} catch { }

if ($null -ne $hiberboot) {
    $state = if ($hiberboot -eq 1) { "on" } else { "off" }
    Say "Fast Startup (HiberbootEnabled): $state"
} else {
    Say "Fast Startup (HiberbootEnabled): value absent (defaults to on where hibernate exists)"
}
Say ""

# --- 2. The service ---------------------------------------------------------
Say "== Service '$ServiceName' =="
Say ""
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not $svc) {
    Say "  NOT INSTALLED"
} else {
    Say ("  Status     : " + $svc.Status)
    Say ("  StartType  : " + $svc.StartType)

    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    try {
        $cfg = Get-ItemProperty -Path $key -ErrorAction Stop
        Say ("  ImagePath  : " + $cfg.ImagePath)
        if ($null -ne $cfg.PreshutdownTimeout) {
            Say ("  Preshutdown: " + $cfg.PreshutdownTimeout + " ms")
        } else {
            Say "  Preshutdown: not set (inherits the 3-minute default)"
        }
    } catch {
        Say "  registry configuration unreadable (needs elevation)"
    }

    if ($elevated) {
        Say "  Recovery   :"
        sc.exe qfailure $ServiceName 2>&1 | ForEach-Object { Say ("    " + $_) }
    }
}
Say ""

# --- 3. The device ----------------------------------------------------------
Say "== ESP32 (VID_$VendorId & PID_$ProductId) =="
Say ""
$hwPattern = "*VID_$VendorId&PID_$ProductId*"
$devices = @(Get-PnpDevice -ErrorAction SilentlyContinue |
             Where-Object { $_.InstanceId -like $hwPattern })

if ($devices.Count -eq 0) {
    Say "  NOT PRESENT — plug the ESP32 in before testing anything below."
} else {
    foreach ($d in $devices) {
        Say ("  " + $d.Status.ToString().PadRight(8) + " " + $d.Class.ToString().PadRight(10) +
             " " + $d.InstanceId)
    }
}
Say ""

# The selective-suspend values the installer writes, per device instance.
if ($elevated) {
    Say "  Selective suspend (per device instance):"
    $enumRoot = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"
    $found = $false
    foreach ($hw in @(Get-ChildItem $enumRoot -ErrorAction SilentlyContinue |
                      Where-Object { $_.PSChildName -like "VID_$VendorId&PID_$ProductId*" })) {
        foreach ($instance in @(Get-ChildItem $hw.PSPath -ErrorAction SilentlyContinue)) {
            $params = Join-Path $instance.PSPath "Device Parameters"
            if (-not (Test-Path $params)) { continue }
            $found = $true
            $p  = Get-ItemProperty -Path $params -ErrorAction SilentlyContinue
            $ss = if ($null -ne $p.SelectiveSuspendEnabled) { $p.SelectiveSuspendEnabled } else { "unset" }
            $ep = if ($null -ne $p.EnhancedPowerManagementEnabled) { $p.EnhancedPowerManagementEnabled } else { "unset" }
            Say ("    " + $instance.PSChildName +
                 "  SelectiveSuspendEnabled=$ss  EnhancedPowerManagementEnabled=$ep")
        }
    }
    if (-not $found) { Say "    no registry instances (device has never been plugged into this machine)" }
    Say ""
}

# --- 4. The log -------------------------------------------------------------
Say "== Daemon log (last $LogLines lines) =="
Say ""
if (Test-Path $logFile) {
    Say ("  $logFile")
    Say ""
    Get-Content $logFile -Tail $LogLines -Encoding UTF8 |
        ForEach-Object { Say ("  " + $_) }
} else {
    Say "  No log at $logFile — the service has never run on this machine."
}
Say ""

# --- 5. What still needs doing by hand --------------------------------------
#
# Things only a person watching the TV can check.
Say "== Manual checks — record the result against this machine =="
Say ""
Say "  [ ] Boot            TV comes on, log shows 'display state = on' at registration"
Say "  [ ] Display timeout TV off when the screen blanks — on every machine"
Say "  [ ] Sleep           TV off; log shows the suspend send and how long it took"
Say "  [ ] Wake            TV on; check whether the ESP32 re-enumerated (arrival line)"
Say "  [ ] Hibernate       as sleep, but 'away for' should show the real elapsed time"
Say "  [ ] Resume from hib TV on when the display comes back"
Say "  [ ] Shutdown        TV off before the machine goes (preshutdown allows 60s)"
Say "  [ ] Unplugged       shutdown is not delayed with the stick removed"
Say "  [ ] Replug          TV does not change when the stick is replugged later"
Say "  [ ] Unattended wake TV stays OFF — this is the 3am case, and the one that"
Say "                      is wrong if PBT_APMRESUMEAUTOMATIC is ever acted on"
Say ""

# --- Save -------------------------------------------------------------------
try {
    $dir = Split-Path -Parent $Report
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    # UTF8, to match the daemon's log.
    $out | Out-File -FilePath $Report -Encoding UTF8
    Write-Host ""
    Write-Host "Report written to $Report"
} catch {
    Write-Warning "Could not write the report to $Report : $_"
}
