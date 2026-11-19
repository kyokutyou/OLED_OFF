param([string]$ExecutablePath = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\SetupTests.exe'))
$ErrorActionPreference = 'Stop'
$executable = (Resolve-Path -LiteralPath $ExecutablePath).Path
$artifacts = Join-Path $PSScriptRoot ('artifacts\monitor lifecycle ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
$children = [Collections.Generic.List[Diagnostics.Process]]::new()
$mutexPrefix = 'Local\OLED_OFF.Lifecycle.' + [Guid]::NewGuid().ToString('N')

function Start-LifecycleChild([string]$Mode, [string]$Name, [string]$MutexName) {
    $log = Join-Path $artifacts ($Name + '.txt')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $child = Start-Process -FilePath $executable -WindowStyle Hidden -PassThru -ArgumentList @(
        '--monitor-lifecycle', $Mode, ('"' + $log + '"'), $MutexName)
    $children.Add($child)
    $null = $child.Handle
    return [pscustomobject]@{ Process = $child; Timer = $timer; Log = $log }
}

function Complete-LifecycleChild($Child, [int]$Timeout = 6000) {
    if (-not $Child.Process.WaitForExit($Timeout)) { throw 'Lifecycle child did not terminate.' }
    $Child.Timer.Stop()
    $Child.Process.Refresh()
}

function Read-Events($Child) {
    if (Test-Path -LiteralPath $Child.Log) { return @([IO.File]::ReadAllLines($Child.Log)) }
    return @()
}

function Assert-SingleCompletedRequest($Child) {
    $events = @(Read-Events $Child)
    if ($Child.Process.ExitCode -ne 0 -or
        @($events | Where-Object { $_ -eq 'dispatch-start' }).Count -ne 1 -or
        @($events | Where-Object { $_ -eq 'dispatch-end' }).Count -ne 1 -or
        $events -notcontains 'window-destroyed' -or $events -notcontains 'completed-ok') {
        throw 'Request did not dispatch exactly once, destroy its local window, and exit successfully.'
    }
}

try {
    $normal = Start-LifecycleChild 'normal' 'normal' ($mutexPrefix + '.normal')
    Complete-LifecycleChild $normal
    Assert-SingleCompletedRequest $normal
    if ($normal.Timer.ElapsedMilliseconds -lt 450 -or $normal.Timer.ElapsedMilliseconds -gt 2500) {
        throw 'Normal request did not finish promptly after its key-release delay.'
    }
    Write-Output ('PASS: normal request dispatched once, destroyed its window, and exited in {0} ms.' -f $normal.Timer.ElapsedMilliseconds)

    $first = Start-LifecycleChild 'hold' 'first' ($mutexPrefix + '.overlap')
    $ready = [Diagnostics.Stopwatch]::StartNew()
    while (@(Read-Events $first) -notcontains 'dispatch-start') {
        if ($first.Process.HasExited -or $ready.ElapsedMilliseconds -gt 2500) {
            throw 'First request did not enter the test dispatcher.'
        }
        Start-Sleep -Milliseconds 10
    }
    $overlap = Start-LifecycleChild 'normal' 'overlap' ($mutexPrefix + '.overlap')
    Complete-LifecycleChild $overlap 1200
    $overlapEvents = @(Read-Events $overlap)
    if ($overlap.Process.ExitCode -ne 0 -or $overlapEvents -contains 'dispatch-start' -or
        $overlapEvents -notcontains 'window-none' -or $first.Process.HasExited) {
        throw 'Overlapping request waited for the first request or dispatched an old request.'
    }
    Complete-LifecycleChild $first
    Assert-SingleCompletedRequest $first
    Write-Output ('PASS: overlapping request exited without dispatch in {0} ms while the first request was still active.' -f $overlap.Timer.ElapsedMilliseconds)

    # A sibling can be created while A is active yet only reach its lock after
    # A exits. A nonwaiting mutex alone cannot distinguish that old request.
    $earlier = Start-LifecycleChild 'normal' 'cohort-first' ($mutexPrefix + '.cohort')
    $ready.Restart()
    while (@(Read-Events $earlier) -notcontains 'child-start') {
        if ($earlier.Process.HasExited -or $ready.ElapsedMilliseconds -gt 1500) { throw 'Cohort fixture did not start.' }
        Start-Sleep -Milliseconds 5
    }
    if ($earlier.Process.HasExited) { throw 'Cohort fixture did not overlap.' }
    $delayed = Start-LifecycleChild 'delayed-overlap' 'cohort-delayed' ($mutexPrefix + '.cohort')
    Complete-LifecycleChild $earlier
    Complete-LifecycleChild $delayed
    Assert-SingleCompletedRequest $earlier
    $delayedEvents = @(Read-Events $delayed)
    if ($delayed.Process.ExitCode -ne 0 -or $delayedEvents -contains 'dispatch-start' -or
        $delayedEvents -notcontains 'window-none') { throw 'An old sibling dispatched after the earlier request completed.' }
    Write-Output 'PASS: a sibling delayed before lock acquisition was discarded after the first request exited.'

    $stale = Start-LifecycleChild 'stale' 'stale' ($mutexPrefix + '.stale')
    Complete-LifecycleChild $stale
    $staleEvents = @(Read-Events $stale)
    if ($stale.Process.ExitCode -ne 0 -or $staleEvents -contains 'dispatch-start' -or
        $staleEvents -notcontains 'window-none' -or $staleEvents -notcontains 'completed-ok') {
        throw 'Expired request was dispatched.'
    }
    Write-Output ('PASS: expired request exited without dispatch in {0} ms.' -f $stale.Timer.ElapsedMilliseconds)

    $lateStart = Start-LifecycleChild 'late-start' 'late-start' ($mutexPrefix + '.late-start')
    Complete-LifecycleChild $lateStart
    $lateEvents = @(Read-Events $lateStart)
    if ($lateStart.Process.ExitCode -ne 0 -or $lateEvents -contains 'dispatch-start' -or
        $lateEvents -notcontains 'window-none' -or $lateEvents -notcontains 'completed-ok') {
        throw 'Request delayed before entering the monitor function was dispatched.'
    }
    Write-Output ('PASS: request delayed before function entry expired without dispatch in {0} ms.' -f $lateStart.Timer.ElapsedMilliseconds)

    $blocked = Start-LifecycleChild 'blocked' 'blocked' ($mutexPrefix + '.blocked')
    Complete-LifecycleChild $blocked
    $blockedEvents = @(Read-Events $blocked)
    if ($blocked.Process.ExitCode -eq 0 -or
        @($blockedEvents | Where-Object { $_ -eq 'dispatch-start' }).Count -ne 1 -or
        $blockedEvents -contains 'dispatch-end' -or $blockedEvents -contains 'completed-ok' -or
        $blocked.Timer.ElapsedMilliseconds -lt 2500 -or $blocked.Timer.ElapsedMilliseconds -gt 5000) {
        throw 'Blocked request was not terminated by the deadline.'
    }
    $before = [IO.File]::ReadAllText($blocked.Log)
    Start-Sleep -Milliseconds 5000
    if ([IO.File]::ReadAllText($blocked.Log) -ne $before -or -not $blocked.Process.HasExited) {
        throw 'Terminated request continued processing after process exit.'
    }
    foreach ($child in $children) {
        if (-not $child.HasExited) { throw 'A lifecycle test process remained running.' }
    }
    Write-Output ('PASS: blocked dispatcher was terminated in {0} ms; no process or later output remained after 5 seconds.' -f $blocked.Timer.ElapsedMilliseconds)
    Write-Output 'PASS: monitor lifetime, expiry, overlap suppression, and blocked-call termination. No display-off API was executed.'
    Write-Output "Test artifacts: $artifacts"
}
finally {
    foreach ($child in $children) {
        try {
            if (-not $child.HasExited) { $child.Kill(); $null = $child.WaitForExit(5000) }
        }
        catch { Write-Warning ("Could not clean up lifecycle child {0}: {1}" -f $child.Id, $_.Exception.Message) }
        finally { $child.Dispose() }
    }
}
