# A release of the Hitman 2 editor (docs: release/README.txt is what players read):
#   build\dist\hitmanlevels-h2-<version>.zip          dinput8.dll, H2Fix.ini, levelpack.exe, README.txt,
#                                                    LICENSE.txt, THIRD_PARTY_LICENSES.txt, examples\
#   build\dist\hitmanlevels-h2-<version>-symbols.zip  dinput8.pdb, dinput8.map (crash logs' addresses)
#   build\dist\SHA256SUMS.txt
# (not build\release: on Windows that is build\Release, the build's own folder)
# The version is CMakeLists.txt's project(... VERSION).
#   powershell -File tools/release.ps1
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
Set-Location $root

$m = Select-String -Path CMakeLists.txt -Pattern 'project\(HitmanLevels VERSION (\d+\.\d+\.\d+)'
if (-not $m) { throw "no version in CMakeLists.txt" }
$version = "v" + $m.Matches[0].Groups[1].Value
$name = "hitmanlevels-h2-$version"

if (-not $NoBuild) {
    cmd /c ".\build.bat Release"
    if ($LASTEXITCODE) { throw "build failed" }
}
$bin = Join-Path $root "build\Release"
foreach ($f in "dinput8.dll", "levelpack.exe", "dinput8.pdb") {
    if (-not (Test-Path "$bin\$f")) { throw "missing $bin\$f" }
}
# the files carry this version (hl_version_resource): a stale build is not released by mistake
foreach ($f in "dinput8.dll", "levelpack.exe") {
    $v = (Get-Item "$bin\$f").VersionInfo.FileVersion
    if ("v$v" -ne $version) { throw "$f is version $v, not ${version}: build again" }
}

$out = Join-Path $root "build\dist"
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\examples" | Out-Null

Copy-Item "$bin\dinput8.dll", "$bin\levelpack.exe" $stage
Copy-Item "h2\H2Fix.ini" $stage
Copy-Item "LICENSE" "$stage\LICENSE.txt"
(Get-Content "release\README.txt" -Raw).Replace("{VERSION}", $version) | Set-Content "$stage\README.txt" -NoNewline
# the example missions played through (h2/docs/playtest_log.md): folders to copy into Mods
foreach ($e in "TheAccountant", "DeadDrop", "EstateRun", "TheInformant", "Arena") {
    Copy-Item -Recurse "h2\examples\missions\$e" "$stage\examples\$e"
}

# the libraries' licenses, from their own files (Lua's notice is in lua.h, nlohmann/json's in json.hpp)
$mit = (Get-Content "third_party\imgui\LICENSE.txt" -Raw) -replace '(?s)^.*?Copyright[^\r\n]*\r?\n', ''
$luaNotice = ((Get-Content "third_party\lua\lua.h" -Raw) -split '\* Copyright \(C\)')[1] -replace '\*+/\s*$', ''
$parts = @(
    "Hitman Levels is built with these libraries; their licenses follow.",
    "", "==== Dear ImGui (third_party/imgui) ====", (Get-Content "third_party\imgui\LICENSE.txt" -Raw),
    "==== ImGuizmo (third_party/imguizmo) ====", (Get-Content "third_party\imguizmo\LICENSE" -Raw),
    "==== MinHook (third_party/minhook) ====", (Get-Content "third_party\minhook\LICENSE.txt" -Raw),
    "==== Monocypher (third_party/monocypher) ====", (Get-Content "third_party\monocypher\LICENCE.md" -Raw),
    "==== Lua (third_party/lua) ====", ("Copyright (C)" + ($luaNotice -replace '(?m)^\* ?', '')),
    "==== JSON for Modern C++ (third_party/json, version 3.11.3) ====",
    ("The MIT License (MIT)`r`n`r`nCopyright (c) 2013-2023 Niels Lohmann <https://nlohmann.me>`r`n" + $mit)
)
($parts -join "`r`n") | Set-Content "$stage\THIRD_PARTY_LICENSES.txt"

$zip = Join-Path $out "$name.zip"
$symbols = Join-Path $out "$name-symbols.zip"
Remove-Item -Force $zip, $symbols -ErrorAction SilentlyContinue
# a zip whose entries' names use "/" as the format wants (Windows PowerShell 5's Compress-Archive and its
# .NET Framework's ZipFile write "\", which other unzippers take as part of the name)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function Write-Zip($zipPath, $files) {  # $files: @{ "name/in/zip" = "C:\file" }
    $archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($k in ($files.Keys | Sort-Object)) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $files[$k], $k, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $archive.Dispose() }
}
$entries = @{}
Get-ChildItem -Recurse -File $stage | ForEach-Object {
    $entries[$_.FullName.Substring($stage.Length + 1).Replace("\", "/")] = $_.FullName
}
Write-Zip $zip $entries
Write-Zip $symbols @{ "dinput8.pdb" = "$bin\dinput8.pdb"; "dinput8.map" = "$bin\dinput8.map" }

$sums = foreach ($f in $zip, $symbols) {
    "{0}  {1}" -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLower(), (Split-Path $f -Leaf)
}
$sums | Set-Content (Join-Path $out "SHA256SUMS.txt")
# the site's Editor & tools page reads this (website/public/tools.html): the zip as published
[ordered]@{
    version = $version
    date = (Get-Date -Format "yyyy-MM-dd")
    file = "$name.zip"
    size = (Get-Item $zip).Length
    sha256 = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    github = "https://github.com/nemannyira/hitmanlevels/releases/tag/h2-$version"
} | ConvertTo-Json | Set-Content (Join-Path $root "website\public\release.json")
$sums
Get-ChildItem $zip, $symbols | Select-Object Name, Length
