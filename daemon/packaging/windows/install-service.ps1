# Installs the ESP32 IR Remote daemon as a Windows Service.
#
# The equivalent of Linux's postinst plus the unit file, minus a package
# manager. This is the "sc create now, MSI at first release" step from the
# brief: enough to install, upgrade and remove the daemon across the machines it
# is being tested on, not enough to hand to somebody else.
#
# Run from an elevated PowerShell. Windows client defaults to an execution
# policy of Restricted, under which no .ps1 runs at all, so the bypass is not
# optional — and -Scope Process confines it to that one shell rather than
# changing the machine:
#
#   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#   .\install-service.ps1 -BinaryPath C:\path\to\esp32-ir-daemon.exe
#
# Or without touching policy at all, from an elevated prompt:
#
#   powershell -ExecutionPolicy Bypass -File .\install-service.ps1 `
#              -BinaryPath C:\path\to\esp32-ir-daemon.exe
#
# Re-running it on a machine that already has the service is an upgrade: the
# running service is stopped and removed before the new one is created. That
# matters because this is installed and reinstalled repeatedly while the three
# power models are being worked through, and an installer that only works on a
# clean machine is one that stops working on exactly the second attempt.
#
# Uninstall:
#   .\install-service.ps1 -Uninstall
#   .\install-service.ps1 -Uninstall -RemoveLogs
#
# That this needs a policy bypass at all is part of why the brief lists an MSI
# as the real answer: an installer a user has to disarm the shell to run is not
# something to hand to anybody else.

[CmdletBinding()]
param(
    [string]$BinaryPath,
    [string]$ServiceName = "esp32-ir-remote",

    # Must match DEVICE_VID / DEVICE_PID in firmware/src/main.cpp and
    # daemon/src/main.cpp. Lowercase or uppercase hex, no 0x. This is one of
    # five hand-kept copies — see the brief, and the argument there for
    # generating all five from one source instead.
    #
    # 1209:0001 is pid.codes' open-hardware VID with its test PID: a
    # placeholder until a real one is allocated.
    #
    # Not named -Pid: $PID is a PowerShell automatic variable holding the
    # process ID, and binding a parameter to it fails in ways that are not
    # obvious from the error.
    [string]$VendorId  = "1209",
    [string]$ProductId = "0001",

    [switch]$Uninstall,

    # Only meaningful with -Uninstall. Off by default: the log is the record of
    # what the daemon did, and an uninstall during testing is usually the
    # prelude to a reinstall rather than the end of the investigation.
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

# Stops and deletes the service, waiting for each step to actually complete.
#
# `sc.exe delete` marks a service for deletion rather than deleting it: while
# any handle remains open — Services.msc being one, and the SCM's own handle
# during a stop being another — the service lingers in a "marked for deletion"
# state where New-Service then fails with a message that does not mention it.
# Waiting for it to disappear is what makes reinstalling reliable.
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

    # Poll for the deletion to land rather than assuming it did.
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

# Undoes the per-device selective-suspend values this installer writes.
#
# Left behind, they would keep suppressing selective suspend for a device whose
# daemon is gone — a machine quietly carrying a setting nothing on it explains.
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

# An upgrade rather than a first install. Done before anything else, because
# every step below either creates the service or configures something the new
# service will read.
if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    Write-Host "'$ServiceName' is already installed — replacing it."
    Remove-DaemonService -Name $ServiceName | Out-Null
}

# --- Create -----------------------------------------------------------------
#
# start= auto rather than delayed-auto. A delayed start would postpone the
# service by up to two minutes, and the first thing it does on starting is read
# the display state and assert the TV to match — so a delayed start is a delayed
# TV, on exactly the boot the feature exists for.
#
# obj= LocalSystem for now. The brief's plan is to move to LocalService once the
# daemon is known to work, mirroring Linux, which ran as root until it did and
# was then moved to the esp32ir account. Whether hidapi's CreateFile open works
# under a low-privilege account is answerable in minutes with a working binary
# and not at all without one.
#
# New-Service rather than `sc.exe create`.
#
# sc.exe parses its own command tail rather than using CommandLineToArgvW, so
# binPath= has to arrive already quoted when the path contains a space — and
# getting a quoted string through PowerShell's native-argument handling
# intact is unreliable enough that it is not worth relying on for the one
# argument whose corruption produces a service that installs cleanly and then
# fails to start with a misleading error.
#
# New-Service takes the path as a real parameter. The embedded quotes below are
# deliberate and are what Windows stores: an unquoted service path with a space
# is the classic unquoted-service-path weakness, where Windows would try
# C:\Program.exe before the real target.
$binaryPathName = '"' + $BinaryPath + '"'

New-Service -Name $ServiceName `
            -BinaryPathName $binaryPathName `
            -DisplayName "ESP32 IR Remote" `
            -Description "Mirrors PC power state to a TV over IR, as a CEC alternative." `
            -StartupType Automatic | Out-Null

# --- Recovery ---------------------------------------------------------------
#
# Mirrors Restart=on-failure / RestartSec=5 on Linux. reset= 86400 means the
# failure count returns to zero after a day without one, so an occasional
# failure does not eventually exhaust the actions.
#
# The brief notes this is still short of what a commercial service specifies:
# there is a decision to make about whether to stop retrying eventually, rather
# than restarting forever.
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/5000 | Out-Null

# --- Preshutdown timeout ----------------------------------------------------
#
# Set explicitly rather than inherited. There is no sc.exe verb for it — the
# documented API is ChangeServiceConfig2 with SERVICE_CONFIG_PRESHUTDOWN_INFO,
# and this registry value is what that writes.
#
# 60s is comfortably more than the shutdown send can take — the daemon gives up
# after 2s — and far less than the 3-minute default, which would hold up a
# shutdown that long if the daemon ever hung. Being explicit is the point: the
# timeout is now a decision rather than whatever the OS defaults to this year.
$key = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
New-ItemProperty -Path $key -Name "PreshutdownTimeout" `
                 -Value 60000 -PropertyType DWord -Force | Out-Null

# --- USB selective suspend --------------------------------------------------
#
# Defence in depth rather than a fix. The firmware now rebuilds its USB state
# after a suspend, so an idle selective suspend is survivable; not entering one
# simply avoids re-enumeration churn on a device that is idle most of the time.
#
# Diagnosed 2026-08-24 on the Modern Standby test laptop. The global power
# scheme setting was not enough — a per-device value overrides it, and on a
# laptop the AC and DC values are separate, so a scheme change made on battery
# power silently does nothing.
#
# It is per-instance: a different physical unit enumerates with a new serial and
# gets Windows' defaults again. The real answer is an INF that carries these
# values for the hardware ID, which arrives with the driver package and code
# signing already listed as gaps.
function Disable-UsbSelectiveSuspend {
    param([string]$VendorHex, [string]$ProductHex)

    $hardwareKey = "VID_$VendorHex&PID_$ProductHex"
    $enumRoot    = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"

    # Collected first, then written. A counter incremented inside a piped
    # ForEach-Object block lands in the wrong scope, and the function would
    # report zero while having done the work — a lie in the one message
    # telling the operator whether the fix was applied.
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
    # Not fatal: the device simply is not plugged in. Saying so is the point —
    # a silent no-op here reappears later as the intermittent write failure,
    # with nothing connecting the two.
    Write-Warning ("No USB device matching VID_{0}&PID_{1} is present, so selective " +
                   "suspend was not disabled for it. Plug the ESP32 in and re-run " +
                   "this script, or writes will start failing once it has been " +
                   "idle." -f $VendorId, $ProductId)
}

# --- Log directory ----------------------------------------------------------
#
# Created here rather than left to the daemon so the ACL is set once, by the
# installer, while it still has the privilege to do it. Matters for the move to
# LocalService: that account cannot create a directory under %ProgramData%
# itself, and a service with nowhere to log fails silently by definition.
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
icacls $logDir /grant "*S-1-5-19:(OI)(CI)M" | Out-Null   # NT AUTHORITY\LOCAL SERVICE
icacls $logDir /grant "*S-1-5-18:(OI)(CI)F" | Out-Null   # NT AUTHORITY\SYSTEM

# --- Start and confirm ------------------------------------------------------
#
# Confirmed rather than assumed. `sc.exe start` returns as soon as the SCM has
# accepted the request, so a service that starts and immediately exits looks
# identical to one that came up — and the failure is then discovered much later,
# by the TV not responding.
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

# The opening lines carry the capability report — which of the three power
# models this machine is — and the first display-state reading. Both are what
# the next steps are read against, so they are shown rather than left to be
# looked up.
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
