param(
    [Parameter(Mandatory = $true)][string]$ExecutablePath,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
if (-not $Run) {
    Write-Output 'No display action was performed. Add -Run to launch the supplied EXE once with --off, automatically wake the display, and observe it for 10 seconds.'
    return
}
$executable = (Resolve-Path -LiteralPath $ExecutablePath).Path
$artifacts = Join-Path $PSScriptRoot ('artifacts\display-integration ' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null

# This observer never sends an off command. Only the supplied EXE performs the
# one requested off action. Recovery first sends mouse input; if Windows does
# not report an on state, it directly requests on using a local helper window.
# The independent C# thread also runs recovery while PowerShell may be waiting.
$source = @'
using System;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

public sealed class OledDisplayPowerProbe : IDisposable {
    public sealed class Event {
        public string Kind { get; set; }
        public int State { get; set; }
        public long ElapsedMilliseconds { get; set; }
        public string Utc { get; set; }
        public string Detail { get; set; }
    }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr WindowProcedure(IntPtr window, uint message, UIntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass {
        public uint style;
        public WindowProcedure procedure;
        public int classExtra, windowExtra;
        public IntPtr instance, icon, cursor, background;
        public string menuName, className;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] private struct Message {
        public IntPtr window;
        public uint message;
        public UIntPtr w;
        public IntPtr l;
        public uint time;
        public Point point;
        public uint privateData;
    }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput {
        public int dx, dy;
        public uint data, flags, time;
        public UIntPtr extraInfo;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Input {
        public uint type;
        public MouseInput mouse;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern ushort RegisterClassW(ref WindowClass type);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool UnregisterClassW(string name, IntPtr instance);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr CreateWindowExW(uint extendedStyle, string className, string name, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll")] private static extern IntPtr DefWindowProcW(IntPtr window, uint message, UIntPtr w, IntPtr l);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr RegisterPowerSettingNotification(IntPtr window, ref Guid setting, uint flags);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool UnregisterPowerSettingNotification(IntPtr notification);
    [DllImport("user32.dll", SetLastError = true)] private static extern int GetMessageW(out Message message, IntPtr window, uint first, uint last);
    [DllImport("user32.dll")] private static extern bool TranslateMessage(ref Message message);
    [DllImport("user32.dll")] private static extern IntPtr DispatchMessageW(ref Message message);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool PostMessageW(IntPtr window, uint message, UIntPtr w, IntPtr l);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern void PostQuitMessage(int exitCode);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, Input[] inputs, int size);

    private static readonly Guid DisplaySetting = new Guid("6FE69556-704A-47A0-8F24-C28D936FDA47");
    private readonly ConcurrentQueue<Event> events = new ConcurrentQueue<Event>();
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private readonly ManualResetEvent ready = new ManualResetEvent(false);
    private readonly Thread observer;
    private readonly WindowProcedure procedure;
    private Thread wakeThread;
    private IntPtr window;
    private volatile int latestDisplayState = -1;
    private Exception observerError;
    private bool disposed;

    public OledDisplayPowerProbe() {
        procedure = ProcessWindowMessage;
        observer = new Thread(Observe);
        observer.IsBackground = true;
        observer.Name = "OLED_OFF display-state observer";
        observer.Start();
        if (!ready.WaitOne(5000)) {
            Dispose();
            throw new TimeoutException("Display-state observer did not start.");
        }
        if (observerError != null) {
            Dispose();
            throw new InvalidOperationException("Could not register display notifications.", observerError);
        }
    }
    public void Mark(string kind, string detail) {
        events.Enqueue(new Event { Kind = kind, State = -1, ElapsedMilliseconds = clock.ElapsedMilliseconds,
            Utc = DateTime.UtcNow.ToString("o"), Detail = detail });
    }
    public Event[] Snapshot() { return events.ToArray(); }
    public string ObserverError { get { return observerError == null ? null : observerError.ToString(); } }
    public static int InputSize { get { return Marshal.SizeOf(typeof(Input)); } }

    private IntPtr ProcessWindowMessage(IntPtr handle, uint message, UIntPtr w, IntPtr l) {
        if (message == 0x0218 && w.ToUInt64() == 0x8013 && l != IntPtr.Zero) {
            Guid setting = (Guid)Marshal.PtrToStructure(l, typeof(Guid));
            if (setting == DisplaySetting && Marshal.ReadInt32(l, 16) >= 4) {
                int state = Marshal.ReadInt32(l, 20);
                latestDisplayState = state;
                events.Enqueue(new Event { Kind = "display-state", State = state,
                    ElapsedMilliseconds = clock.ElapsedMilliseconds, Utc = DateTime.UtcNow.ToString("o"),
                    Detail = state == 0 ? "off" : state == 1 ? "on" : state == 2 ? "dimmed" : "unknown" });
            }
            return new IntPtr(1);
        }
        if (message == 0x0010) { DestroyWindow(handle); return IntPtr.Zero; }
        if (message == 0x0002) { window = IntPtr.Zero; PostQuitMessage(0); return IntPtr.Zero; }
        return DefWindowProcW(handle, message, w, l);
    }
    private void Observe() {
        string className = "OLED_OFF.DisplayProbe." + Guid.NewGuid().ToString("N");
        IntPtr instance = GetModuleHandleW(null), notification = IntPtr.Zero;
        bool registered = false;
        try {
            WindowClass type = new WindowClass { procedure = procedure, instance = instance, className = className };
            if (RegisterClassW(ref type) == 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "RegisterClassW");
            registered = true;
            window = CreateWindowExW(0, className, "OLED_OFF display-state observer", 0, 0, 0, 0, 0,
                IntPtr.Zero, IntPtr.Zero, instance, IntPtr.Zero);
            if (window == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateWindowExW");
            Guid setting = DisplaySetting;
            notification = RegisterPowerSettingNotification(window, ref setting, 0);
            if (notification == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "RegisterPowerSettingNotification");
            Mark("observer-ready", "GUID_CONSOLE_DISPLAY_STATE");
            ready.Set();
            Message message;
            int status;
            while ((status = GetMessageW(out message, IntPtr.Zero, 0, 0)) > 0) {
                TranslateMessage(ref message); DispatchMessageW(ref message);
            }
            if (status == -1) throw new Win32Exception(Marshal.GetLastWin32Error(), "GetMessageW");
        } catch (Exception error) {
            observerError = error;
            Mark("observer-error", error.Message);
        } finally {
            if (notification != IntPtr.Zero && !UnregisterPowerSettingNotification(notification))
                Mark("cleanup-error", "UnregisterPowerSettingNotification: " + Marshal.GetLastWin32Error());
            if (window != IntPtr.Zero) { DestroyWindow(window); window = IntPtr.Zero; }
            if (registered && !UnregisterClassW(className, instance))
                Mark("cleanup-error", "UnregisterClassW: " + Marshal.GetLastWin32Error());
            ready.Set();
        }
    }
    public int Wake(string reason) {
        int size = InputSize;
        int expected = IntPtr.Size == 8 ? 40 : 28;
        if (size != expected) throw new InvalidOperationException("Unexpected INPUT size: " + size);
        // Keep opposite motions in distinct, noncoalesced input events. A
        // single array containing +1 and -1 can otherwise disappear as zero
        // net motion before it produces a useful wake action.
        Input[] input = new Input[1];
        input[0].mouse.dx = 1; input[0].mouse.flags = 0x0001 | 0x2000;
        uint first = SendInput(1, input, size);
        int firstError = first == 1 ? 0 : Marshal.GetLastWin32Error();
        Thread.Sleep(50);
        input[0].mouse.dx = -1;
        uint second = SendInput(1, input, size);
        int secondError = second == 1 ? 0 : Marshal.GetLastWin32Error();
        uint sent = first + second;
        Mark("wake", reason + "; inserted=" + sent + "; firstWin32=" + firstError + "; secondWin32=" + secondError);
        return (int)sent;
    }
    private bool WaitForDisplayOn(int milliseconds) {
        long limit = clock.ElapsedMilliseconds + milliseconds;
        while (latestDisplayState != 1 && clock.ElapsedMilliseconds < limit) Thread.Sleep(25);
        return latestDisplayState == 1;
    }
    public int Recover(string reason) {
        int sent = Wake(reason);
        if (WaitForDisplayOn(750)) return sent;

        // Recovery only: this never requests off, broadcasts, or leaves an
        // on request in another application's queue. The caller owns this
        // hidden window and executes DefWindowProc directly on the same thread.
        IntPtr localWindow = CreateWindowExW(0, "STATIC", "OLED_OFF test recovery", 0x80000000,
            0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, GetModuleHandleW(null), IntPtr.Zero);
        if (localWindow == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create recovery window.");
        try {
            Mark("wake-power-on-fallback", reason + "; input did not produce an on notification");
            IntPtr result = DefWindowProcW(localWindow, 0x0112, new UIntPtr(0xF170), new IntPtr(-1));
            Mark("wake-power-on-result", "DefWindowProcW=" + result);
        } finally {
            DestroyWindow(localWindow);
        }
        Mark("wake-recovery-state", WaitForDisplayOn(1500) ? "on confirmed" : "on not confirmed");
        return sent;
    }
    public void ScheduleSafetyWake() {
        if (wakeThread != null) throw new InvalidOperationException("Safety wake already scheduled.");
        wakeThread = new Thread(delegate() {
            Thread.Sleep(6000);
            try { Recover("independent six-second safety wake"); }
            catch (Exception error) { Mark("wake-error", error.ToString()); }
        });
        wakeThread.IsBackground = true;
        wakeThread.Name = "OLED_OFF independent safety wake";
        wakeThread.Start();
    }
    public void Dispose() {
        if (disposed) return;
        disposed = true;
        // Do not cancel the safety wake on an error path. It must run even if
        // the PowerShell orchestration threw after launching the off process.
        if (wakeThread != null && !wakeThread.Join(8000))
            throw new TimeoutException("Safety-wake thread did not stop.");
        IntPtr handle = window;
        if (handle != IntPtr.Zero && !PostMessageW(handle, 0x0010, UIntPtr.Zero, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not close observer window.");
        if (!observer.Join(5000)) throw new TimeoutException("Display-state observer did not stop.");
        ready.Dispose();
        GC.KeepAlive(procedure);
    }
}
'@
if (-not ('OledDisplayPowerProbe' -as [type])) { Add-Type -TypeDefinition $source }

$probe = $null
$child = $null
$processTimer = $null
$failure = $null
$report = [ordered]@{
    Executable = $executable
    Status = 'INCOMPLETE'
    ProcessId = $null
    ExitCode = $null
    ProcessElapsedMilliseconds = $null
    OffObserved = $false
    OnObservedAfterOff = $false
    NoFurtherOffForTenSeconds = $false
    ChildExited = $false
    ExplicitWakeFallbackUsed = $false
    InputStructureSize = [OledDisplayPowerProbe]::InputSize
    Error = $null
    Events = @()
    Note = 'Display states are Windows notifications; this does not measure physical panel light output.'
}
try {
    $probe = [OledDisplayPowerProbe]::new()
    $baselineTimer = [Diagnostics.Stopwatch]::StartNew()
    while (@($probe.Snapshot() | Where-Object { $_.Kind -eq 'display-state' -and $_.State -eq 1 }).Count -eq 0) {
        if ($baselineTimer.ElapsedMilliseconds -gt 3000 -or $probe.ObserverError) {
            throw 'INCONCLUSIVE: No initial display-on notification was received. The EXE was not launched.'
        }
        Start-Sleep -Milliseconds 25
    }
    $probe.ScheduleSafetyWake()
    $probe.Mark('launch', 'Exactly one --off invocation follows.')
    $launchTime = @($probe.Snapshot() | Where-Object Kind -eq 'launch')[-1].ElapsedMilliseconds
    $processTimer = [Diagnostics.Stopwatch]::StartNew()
    $child = Start-Process -FilePath $executable -ArgumentList '--off' -WindowStyle Hidden -PassThru
    $report.ProcessId = $child.Id
    $null = $child.Handle
    if (-not $child.WaitForExit(5000)) { throw 'FAIL: Monitor-off child remained running for more than five seconds.' }
    $processTimer.Stop()
    $child.Refresh()
    $report.ChildExited = $child.HasExited
    $report.ExitCode = $child.ExitCode
    $report.ProcessElapsedMilliseconds = $processTimer.ElapsedMilliseconds
    $probe.Mark('child-exit', ('exit={0}; elapsed={1} ms' -f $child.ExitCode, $processTimer.ElapsedMilliseconds))
    if ($child.ExitCode -ne 0) { throw 'FAIL: Monitor-off EXE returned a nonzero exit code.' }

    # Allow the OS to deliver off notifications before restoring the display.
    Start-Sleep -Milliseconds 2000
    if ($probe.Wake('main test wake') -ne 2) { throw 'INCONCLUSIVE: SendInput did not insert both wake events.' }
    $onTimer = [Diagnostics.Stopwatch]::StartNew()
    $onAfterOff = $null
    while ($onTimer.ElapsedMilliseconds -lt 5000) {
        $events = @($probe.Snapshot())
        $off = @($events | Where-Object { $_.Kind -eq 'display-state' -and $_.State -eq 0 -and $_.ElapsedMilliseconds -ge $launchTime })
        if ($off.Count -gt 0) {
            $report.OffObserved = $true
            $onAfterOff = @($events | Where-Object { $_.Kind -eq 'display-state' -and $_.State -eq 1 -and $_.ElapsedMilliseconds -gt $off[0].ElapsedMilliseconds }) | Select-Object -First 1
            if ($null -ne $onAfterOff) { break }
        }
        Start-Sleep -Milliseconds 25
    }
    if (-not $report.OffObserved) { throw 'INCONCLUSIVE: No display-off notification was received; successful extinction cannot be asserted.' }
    if ($null -eq $onAfterOff) { throw 'INCONCLUSIVE: No display-on notification followed the off notification.' }
    $report.OnObservedAfterOff = $true
    Start-Sleep -Milliseconds 10000
    $laterOff = @($probe.Snapshot() | Where-Object { $_.Kind -eq 'display-state' -and $_.State -eq 0 -and $_.ElapsedMilliseconds -gt $onAfterOff.ElapsedMilliseconds })
    if ($laterOff.Count -gt 0) { throw 'FAIL: Another display-off notification occurred after the display returned on.' }
    if (-not $child.HasExited) { throw 'FAIL: Monitor-off child remained running.' }
    if ($probe.ObserverError) { throw ('INCONCLUSIVE: Observer failed: ' + $probe.ObserverError) }
    $report.NoFurtherOffForTenSeconds = $true
    $report.Status = 'PASS'
}
catch {
    $failure = $_
    $report.Error = $_.Exception.Message
    $report.Status = if ($_.Exception.Message -like 'INCONCLUSIVE:*') { 'INCONCLUSIVE' } else { 'FAIL' }
}
finally {
    if ($null -ne $child) {
        try {
            if (-not $child.HasExited) { $child.Kill(); $null = $child.WaitForExit(5000) }
            $report.ChildExited = $child.HasExited
            if ($child.HasExited) { $report.ExitCode = $child.ExitCode }
        }
        catch { $report.Status = 'FAIL'; $report.Error = 'Could not clean up child: ' + $_.Exception.Message }
        finally { $child.Dispose() }
    }
    if ($null -ne $probe) {
        try {
            if ($null -ne $processTimer) {
                if ($probe.Recover('finally recovery wake') -ne 2) {
                    $report.Status = 'INCONCLUSIVE'; $report.Error = 'Recovery SendInput did not insert both events.'
                }
            }
        }
        catch { $report.Status = 'INCONCLUSIVE'; $report.Error = 'Recovery wake failed: ' + $_.Exception.Message }
        finally {
            try { $probe.Dispose() }
            catch { $report.Status = 'FAIL'; $report.Error = 'Observer cleanup failed: ' + $_.Exception.Message }
            $report.Events = @($probe.Snapshot() | Sort-Object ElapsedMilliseconds)
            $report.ExplicitWakeFallbackUsed = @($report.Events | Where-Object Kind -eq 'wake-power-on-fallback').Count -gt 0
            if (@($report.Events | Where-Object { $_.Kind -in @('cleanup-error', 'wake-error') }).Count -gt 0) {
                $report.Status = 'FAIL'; $report.Error = 'Observer or safety-wake cleanup reported an error.'
            }
        }
    }
    $json = $report | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText((Join-Path $artifacts 'result.json'), $json, [Text.UTF8Encoding]::new($false))
    Write-Output $json
    Write-Output "Test artifacts: $artifacts"
}
if ($report.Status -ne 'PASS') {
    if ($null -ne $failure) { throw $failure }
    throw $report.Error
}
