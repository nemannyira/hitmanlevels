# Install the freshly built DLL, launch the game through Steam, capture
# timed screenshots and copy H2Fix.log next to them.
#   run_game.ps1 -Seconds 60 -Interval 2 -Out <dir> [-Config Release] [-NoInstall] [-Keep]
param(
    [int]$Seconds = 60,
    [double]$Interval = 2,
    [string]$Out = "$PSScriptRoot\..\dumps\run",
    [string]$Config = "Release",
    [switch]$NoInstall,
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
$game = "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

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
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds ([int]($Interval * 1000))
    if (-not (Get-Process hitman2 -ErrorAction SilentlyContinue)) { continue }
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $small = New-Object System.Drawing.Bitmap $bmp, 200, 150
    $t = [int]((Get-Date) - $t0).TotalSeconds
    $small.Save(("{0}\{1:D3}s.png" -f $Out, $t))
    $g.Dispose(); $bmp.Dispose(); $small.Dispose()
    $i++
}
Copy-Item "$game\H2Fix.log" "$Out\H2Fix.log" -ErrorAction SilentlyContinue
if (-not $Keep) { Get-Process hitman2 -ErrorAction SilentlyContinue | Stop-Process -Force }
"captured $i screenshots"
