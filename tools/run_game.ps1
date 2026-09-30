# Install the freshly built DLL, launch the game through Steam, capture
# timed screenshots and copy H2Fix.log next to them.
#   run_game.ps1 -Seconds 60 -Interval 2 -Out <dir> [-Config Release] [-NoInstall] [-Keep]
param(
    [int]$Seconds = 60,
    [double]$Interval = 2,
    [string]$Out = "$PSScriptRoot\..\dumps\run",
    [string]$Config = "Release",
    [switch]$NoInstall,
    [switch]$Keep,
    [switch]$Full,  # also keep full resolution screenshots (*_full.png)
    [string]$EscAt = "",    # comma separated seconds after launch at which to press Escape (skip intros)
    [string]$Keys = ""      # key script, e.g. "7:esc,9:esc,12:enter,20:w+3" (key+holdSeconds)
)
$ErrorActionPreference = "Stop"
$game = "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System.Runtime.InteropServices;
public static class Keys {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
    [DllImport("user32.dll")] static extern System.IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
    public static uint ForegroundPid() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }
    // scan-code events so DirectInput sees them
    public static void Down(byte scan) { keybd_event(0, scan, 0x0008, System.UIntPtr.Zero); }
    public static void Up(byte scan) { keybd_event(0, scan, 0x0008 | 0x0002, System.UIntPtr.Zero); }
    public static void Tap(byte scan) { Down(scan); System.Threading.Thread.Sleep(60); Up(scan); }
}
"@

Get-Process hitman2 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
if (-not $NoInstall) {
    Copy-Item "$PSScriptRoot\..\build\$Config\dinput8.dll" "$game\dinput8.dll" -Force
}
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Filter *.png -ErrorAction SilentlyContinue | Remove-Item

Start-Process "steam://run/6850"
$t0 = Get-Date
$i = 0
$scan = @{ esc = 0x01; enter = 0x1C; space = 0x39; up = 0x48; down = 0x50; left = 0x4B; right = 0x4D;
          w = 0x11; a = 0x1E; s = 0x1F; d = 0x20; i = 0x17; m = 0x32; tab = 0x0F }
$events = [System.Collections.Generic.List[object]]::new()
if ($EscAt) { $EscAt.Split(",") | ForEach-Object { $events.Add(@{ t = [double]$_; key = "esc"; hold = 0 }) } }
if ($Keys) {
    foreach ($k in $Keys.Split(",")) {
        $t, $rest = $k.Split(":"); $name, $hold = $rest.Split("+")
        $events.Add(@{ t = [double]$t; key = $name; hold = [double]("0" + $hold) })
    }
}
$sorted = @($events | Sort-Object { $_.t })
"key events: $($sorted.Count)"
$events = [System.Collections.Generic.List[object]]::new()
foreach ($e in $sorted) { $events.Add($e) }
$held = @{}
$nextShot = $Interval
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 100
    $now = ((Get-Date) - $t0).TotalSeconds
    # Never type into anything but the game: keys are only sent while hitman2 owns the foreground.
    $gameProc = Get-Process hitman2 -ErrorAction SilentlyContinue | Select-Object -First 1
    $gameFocused = $gameProc -and ([Keys]::ForegroundPid() -eq [uint32]$gameProc.Id)
    if (-not $gameFocused) {
        foreach ($sc in @($held.Keys)) { [Keys]::Up([byte]$sc); $held.Remove($sc) }
        if ($events.Count -gt 0 -and $now -ge $events[0].t) { "skipped key '$($events[0].key)' at $([int]$now)s: game not in foreground" }
        while ($events.Count -gt 0 -and $now -ge $events[0].t) { $events.RemoveAt(0) }
    }
    while ($gameFocused -and $events.Count -gt 0 -and $now -ge $events[0].t) {
        $e = $events[0]; $events.RemoveAt(0); $sc = [byte]$scan[$e.key]
        if ($e.hold -gt 0) { [Keys]::Down($sc); $held[$sc] = $now + $e.hold } else { [Keys]::Tap($sc) }
        "sent key '$($e.key)' at $([math]::Round($now, 1))s"
    }
    foreach ($sc in @($held.Keys)) { if ($now -ge $held[$sc]) { [Keys]::Up([byte]$sc); $held.Remove($sc) } }
    if ($now -lt $nextShot) { continue }
    $nextShot += $Interval
    if (-not (Get-Process hitman2 -ErrorAction SilentlyContinue)) { continue }
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $small = New-Object System.Drawing.Bitmap $bmp, 200, ([int](200 * $b.Height / $b.Width))
    $t = [int]((Get-Date) - $t0).TotalSeconds
    $small.Save(("{0}\{1:D3}s.png" -f $Out, $t))
    if ($Full) { $bmp.Save(("{0}\{1:D3}s_full.png" -f $Out, $t)) }
    $g.Dispose(); $bmp.Dispose(); $small.Dispose()
    $i++
}
if (-not $Keep) {
    Get-Process hitman2 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}
Copy-Item "$game\H2Fix.log" "$Out\H2Fix.log" -ErrorAction SilentlyContinue
"captured $i screenshots"
