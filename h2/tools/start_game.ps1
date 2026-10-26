# Starts Hitman 2 through Steam (h2/tests/run_all.py): answers Steam's launch-option
# dialog with Play, closes a "Steam Service Error" box with Cancel (no service install) and
# answers "Unable to Sync" with Play anyway (the user's choice, Steam Cloud off).
# Prints "game up" once the game's window is there.
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(string title) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
      if (IsWindowVisible(h) && sb.ToString() == title) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern uint GetPixel(IntPtr dc, int x, int y);
  public static uint Pixel(int x, int y) { IntPtr dc = GetDC(IntPtr.Zero); uint c = GetPixel(dc, x, y); ReleaseDC(IntPtr.Zero, dc); return c; }
  public static void Click(int x, int y) { SetCursorPos(x, y); System.Threading.Thread.Sleep(250);
    mouse_event(2, 0, 0, 0, 0); System.Threading.Thread.Sleep(100); mouse_event(4, 0, 0, 0, 0); }
}
'@
Start-Process "steam://run/6850"
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Milliseconds 700
  $g = Get-Process hitman2 -ErrorAction SilentlyContinue
  if ($g -and $g.MainWindowTitle -eq 'Hitman2') { "game up"; exit 0 }
  $svc = [W]::Find('Steam Service Error')
  if ($svc -ne [IntPtr]::Zero) {
    $r = New-Object W+RECT; [W]::GetWindowRect($svc, [ref]$r) | Out-Null; [W]::SetForegroundWindow($svc) | Out-Null
    [W]::Click($r.L + 448, $r.T + 114)  # Cancel
    continue
  }
  # Steam's "Unable to Sync" box (drawn in the middle of Steam's window, no window of its own): its
  # "Play anyway" (the user's choice with Steam Cloud off, 2026-10-26), when its blue is there
  $steam = [W]::Find('Steam')
  if ($i -ge 6 -and $steam -ne [IntPtr]::Zero) {
    $r = New-Object W+RECT; [W]::GetWindowRect($steam, [ref]$r) | Out-Null
    $x = [int](($r.L + $r.R) / 2) + 105; $y = [int](($r.T + $r.B) / 2) + 90
    $c = [W]::Pixel($x - 53, $y - 10); $red = $c -band 0xFF; $blue = ($c -shr 16) -band 0xFF  # (beside its text)
    if ($blue -gt 180 -and $red -lt 80) {
      [W]::SetForegroundWindow($steam) | Out-Null
      [W]::Click($x, $y)  # Play anyway
      Start-Sleep -Seconds 2
      continue
    }
  }
  $dlg = [W]::Find('Hitman 2: Silent Assassin')
  if ($dlg -ne [IntPtr]::Zero) {
    $r = New-Object W+RECT; [W]::GetWindowRect($dlg, [ref]$r) | Out-Null; [W]::SetForegroundWindow($dlg) | Out-Null
    [W]::Click($r.R - 189, $r.B - 33)  # Play
    Start-Sleep -Seconds 2
  }
}
"game did not start"
