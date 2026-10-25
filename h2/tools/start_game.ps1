# Starts Hitman 2 through Steam (h2/tests/run_all.py): answers Steam's launch-option
# dialog with Play and closes a "Steam Service Error" box with Cancel (no service install).
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
  $dlg = [W]::Find('Hitman 2: Silent Assassin')
  if ($dlg -ne [IntPtr]::Zero) {
    $r = New-Object W+RECT; [W]::GetWindowRect($dlg, [ref]$r) | Out-Null; [W]::SetForegroundWindow($dlg) | Out-Null
    [W]::Click($r.R - 189, $r.B - 33)  # Play
    Start-Sleep -Seconds 2
  }
}
"game did not start"
