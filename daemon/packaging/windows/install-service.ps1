# Installs the ESP32 IR Remote daemon as a Windows Service. Re-running it
# replaces an existing install.
#
# Run from an elevated PowerShell. Windows blocks .ps1 scripts by default, so:
#
#   powershell -ExecutionPolicy Bypass -File .\install-service.ps1 `
#              -BinaryPath C:\path\to\esp32-ir-daemon.exe
#
# Uninstall:
#   .\install-service.ps1 -Uninstall [-RemoveLogs]

[CmdletBinding()]
param(
    [string]$BinaryPath,
    [string]$ServiceName = "esp32-ir-remote",

    # One of five copies of the USB ID that must change together (see the
    # brief). Hex, no 0x. Never name anything $Pid: PowerShell reserves $PID.
    [string]$VendorId  = "1209",
    [string]$ProductId = "0001",

    [switch]$Uninstall,

    # Only with -Uninstall. Logs are kept by default.
    [switch]$RemoveLogs
)

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal] `
          [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run from an elevated PowerShell session."
}

$logDir = Join-Path $env:ProgramData "ESP32IRRemote"

# --- Shared helpers ---------------------------------------------------------

# Stops and deletes the service, and waits until it is really gone: while any
# handle is open (Services.msc, say) Windows only marks it for deletion, and
# New-Service would then fail with a misleading error.
function Remove-DaemonService {
    param([string]$Name)

    $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if (-not $svc) { return $false }

    if ($svc.Status -ne 'Stopped') {
        Write-Host "Stopping '$Name'..."
        Stop-Service -Name $Name -Force -ErrorAction SilentlyContinue
        try {
            (Get-Service -Name $Name).WaitForStatus('Stopped', (New-TimeSpan -Seconds 30))
        } catch {
            Write-Warning "'$Name' did not stop within 30s; deleting anyway."
        }
    }

    sc.exe delete $Name | Out-Null

    for ($i = 0; $i -lt 30; $i++) {
        if (-not (Get-Service -Name $Name -ErrorAction SilentlyContinue)) {
            Write-Host "Removed service '$Name'."
            return $true
        }
        Start-Sleep -Milliseconds 500
    }

    throw ("Service '$Name' is still present after deletion. Close Services.msc " +
           "and any Event Viewer or Task Manager window holding a handle to it, " +
           "then re-run.")
}

# Undoes the per-device selective-suspend values written at install.
function Reset-UsbSelectiveSuspend {
    param([string]$VendorHex, [string]$ProductHex)

    $hardwareKey = "VID_$VendorHex&PID_$ProductHex"
    $enumRoot    = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"
    $cleared     = 0

    foreach ($hw in @(Get-ChildItem $enumRoot -ErrorAction SilentlyContinue |
                      Where-Object { $_.PSChildName -like "$hardwareKey*" })) {
        foreach ($instance in @(Get-ChildItem $hw.PSPath -ErrorAction SilentlyContinue)) {
            $params = Join-Path $instance.PSPath "Device Parameters"
            if (-not (Test-Path $params)) { continue }
            foreach ($name in @("SelectiveSuspendEnabled", "EnhancedPowerManagementEnabled")) {
                if ($null -ne (Get-ItemProperty -Path $params -Name $name -ErrorAction SilentlyContinue)) {
                    Remove-ItemProperty -Path $params -Name $name -ErrorAction SilentlyContinue
                    $cleared++
                }
            }
        }
    }
    return $cleared
}

# --- Uninstall --------------------------------------------------------------

if ($Uninstall) {
    $removed = Remove-DaemonService -Name $ServiceName
    if (-not $removed) {
        Write-Host "No service named '$ServiceName' was installed."
    }

    $cleared = Reset-UsbSelectiveSuspend -VendorHex $VendorId -ProductHex $ProductId
    if ($cleared -gt 0) {
        Write-Host "Cleared $cleared selective-suspend registry value(s)."
    }

    if ($RemoveLogs) {
        if (Test-Path $logDir) {
            Remove-Item -Path $logDir -Recurse -Force
            Write-Host "Removed $logDir."
        }
    } else {
        Write-Host "Left the log directory $logDir in place (-RemoveLogs to delete it)."
    }
    return
}

# --- Install ----------------------------------------------------------------

if (-not $BinaryPath) { throw "-BinaryPath is required when installing." }
if (-not (Test-Path $BinaryPath)) { throw "No binary at $BinaryPath" }
$BinaryPath = (Resolve-Path $BinaryPath).Path

# An upgrade: remove the old service first.
if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    Write-Host "'$ServiceName' is already installed — replacing it."
    Remove-DaemonService -Name $ServiceName | Out-Null
}

# --- Create -----------------------------------------------------------------
#
# Automatic, not delayed: a delayed start would delay the TV coming on at boot.
# Runs as LocalSystem for now; the brief plans a move to LocalService.
#
# New-Service rather than sc.exe create, which mangles quoted paths. The quotes
# are deliberate: an unquoted path with a space is a known Windows weakness.
$binaryPathName = '"' + $BinaryPath + '"'

New-Service -Name $ServiceName `
            -BinaryPathName $binaryPathName `
            -DisplayName "ESP32 IR Remote" `
            -Description "Mirrors PC power state to a TV over IR, as a CEC alternative." `
            -StartupType Automatic | Out-Null

# --- Recovery ---------------------------------------------------------------
#
# Restart after 5s, like Linux. The failure count resets after a day.
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/5000 | Out-Null

# --- Preshutdown timeout ----------------------------------------------------
#
# How long shutdown waits for the OFF. 60s: far more than a send takes (2s at
# most), far less than the 3-minute default if the daemon ever hung. There is
# no sc.exe verb for it, so the registry value is written directly.
$key = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
New-ItemProperty -Path $key -Name "PreshutdownTimeout" `
                 -Value 60000 -PropertyType DWord -Force | Out-Null

# --- USB selective suspend --------------------------------------------------
#
# Stops Windows suspending the idle stick while the PC is awake. Firmware built
# on Arduino core 2.x froze after any USB suspend until it was replugged; core
# 3.x does not, so this goes once a Windows machine has confirmed it (see the
# brief, "USB after sleep"). Per device instance, so a different physical stick
# needs the installer re-run.
function Disable-UsbSelectiveSuspend {
    param([string]$VendorHex, [string]$ProductHex)

    $hardwareKey = "VID_$VendorHex&PID_$ProductHex"
    $enumRoot    = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"

    # Collected first, then counted: a counter inside a pipeline would stay 0.
    $paramKeys = @()
    foreach ($hw in @(Get-ChildItem $enumRoot -ErrorAction SilentlyContinue |
                      Where-Object { $_.PSChildName -like "$hardwareKey*" })) {
        foreach ($instance in @(Get-ChildItem $hw.PSPath -ErrorAction SilentlyContinue)) {
            $params = Join-Path $instance.PSPath "Device Parameters"
            if (Test-Path $params) { $paramKeys += $params }
        }
    }

    foreach ($params in $paramKeys) {
        New-ItemProperty -Path $params -Name "SelectiveSuspendEnabled" `
                         -Value 0 -PropertyType DWord -Force | Out-Null
        New-ItemProperty -Path $params -Name "EnhancedPowerManagementEnabled" `
                         -Value 0 -PropertyType DWord -Force | Out-Null
    }

    return $paramKeys.Count
}

$suspendApplied = Disable-UsbSelectiveSuspend -VendorHex $VendorId -ProductHex $ProductId
if ($suspendApplied -gt 0) {
    Write-Host "Disabled USB selective suspend on $suspendApplied device instance(s)."
    Write-Host "Unplug and replug the ESP32 for that to take effect."
} else {
    Write-Warning ("No USB device matching VID_{0}&PID_{1} is present, so selective " +
                   "suspend was not disabled for it. Plug the ESP32 in and re-run " +
                   "this script, or writes will start failing once it has been " +
                   "idle." -f $VendorId, $ProductId)
}

# --- Log directory ----------------------------------------------------------
#
# Created here, with LocalService allowed to write, ready for the move to that
# account, which cannot create it itself.
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
icacls $logDir /grant "*S-1-5-19:(OI)(CI)M" | Out-Null   # NT AUTHORITY\LOCAL SERVICE
icacls $logDir /grant "*S-1-5-18:(OI)(CI)F" | Out-Null   # NT AUTHORITY\SYSTEM

# --- Start and confirm ------------------------------------------------------
#
# Wait for Running, so a service that starts and exits at once is caught here.
Start-Service -Name $ServiceName

try {
    (Get-Service -Name $ServiceName).WaitForStatus('Running', (New-TimeSpan -Seconds 30))
    Write-Host "Installed and started '$ServiceName'."
} catch {
    throw ("'$ServiceName' was installed but did not reach Running within 30s. " +
           "Check $logDir\daemon.log.")
}

Write-Host "Log: $logDir\daemon.log"
Write-Host ""

# Show the first log lines, including the first display-state reading.
$logFile = Join-Path $logDir "daemon.log"
if (Test-Path $logFile) {
    Start-Sleep -Seconds 2
    Write-Host "--- opening log lines ---"
    Get-Content $logFile -Tail 20 -Encoding UTF8
    Write-Host "-------------------------"
    Write-Host ""
}

Write-Host "Follow it with:"
Write-Host "  Get-Content '$logFile' -Wait -Tail 20 -Encoding UTF8"
