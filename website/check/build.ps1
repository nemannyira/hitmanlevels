# The site's upload check built to WebAssembly (check_wasm.cpp): the client's package reader and Hitman 2's
# strict level check, as the editor and levelpack have them, into website/src/check.wasm.
#   powershell -File website/check/build.ps1 [-WasiSdk C:\path\to\wasi-sdk-34.0-x86_64-windows]
param([string]$WasiSdk = "$env:USERPROFILE\tools\wasi-sdk-34.0-x86_64-windows")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
$clang = Join-Path $WasiSdk "bin\clang++.exe"
$cc = Join-Path $WasiSdk "bin\clang.exe"
if (-not (Test-Path $clang)) { throw "wasi-sdk not found: $WasiSdk" }
$out = Join-Path $root "build\wasm"
New-Item -ItemType Directory -Force $out | Out-Null

$common = @("--target=wasm32-wasip1", "-O2", "-fwasm-exceptions", "-mllvm", "-wasm-use-legacy-eh=false", "-I$root\shared", "-I$root\h2\src",
            "-I$root\third_party\json", "-I$root\third_party\monocypher")
$objects = @()
foreach ($c in @("third_party\monocypher\monocypher.c", "third_party\monocypher\monocypher-ed25519.c")) {
    $o = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($c) + ".o")
    & $cc --target=wasm32-wasip1 -O2 -c "$root\$c" -o $o
    if ($LASTEXITCODE) { throw "compile failed: $c" }
    $objects += $o
}
$sources = @("shared\levelfmt\names.cpp", "shared\levelfmt\games.cpp", "shared\levelfmt\crypto.cpp",
             "shared\levelfmt\ed25519.cpp", "shared\levelfmt\package.cpp", "h2\src\levelfmt\h2_game.cpp",
             "h2\src\levelfmt\level_check.cpp", "website\check\check_wasm.cpp")
foreach ($s in $sources) {
    $o = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($s) + ".o")
    & $clang @common -std=c++17 -c "$root\$s" -o $o
    if ($LASTEXITCODE) { throw "compile failed: $s" }
    $objects += $o
}
# a reactor (no main): the Worker calls its exports; exceptions as WebAssembly's own (libunwind)
& $clang --target=wasm32-wasip1 -O2 -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false -mexec-model=reactor -lunwind "-Wl,--strip-all" @objects -o "$root\website\src\check.wasm"
if ($LASTEXITCODE) { throw "link failed" }
Get-Item "$root\website\src\check.wasm" | Select-Object Name, Length
