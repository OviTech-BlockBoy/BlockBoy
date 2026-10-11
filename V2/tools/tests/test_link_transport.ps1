param([string]$Zig = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$out = Join-Path $root 'build/link-host-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source = Get-Content -Raw "$root/components/retro-go/libs/netplay/rg_netplay.c"
$start = $source.IndexOf('static void link_tx_flush(void)')
$end = $source.IndexOf('void rg_netplay_close_serial(void)', $start)
$functions = $source.Substring($start, $end - $start)
$start = $source.IndexOf('static void handle_packet(')
$end = $source.IndexOf('// ─── Main task', $start)
$functions += $source.Substring($start, $end - $start)
$harness = Get-Content -Raw "$PSScriptRoot/link_transport.c"
[IO.File]::WriteAllText("$out/link_test.c", $harness.Replace('/* SOURCE_FUNCTIONS */', $functions))
$env:ZIG_GLOBAL_CACHE_DIR = "$out/zig-cache"
$env:ZIG_LOCAL_CACHE_DIR = "$out/zig-local-cache"
if (!$Zig) { $Zig = "$root/build/gba-test-toolchain/ziglang/zig.exe" }
& $Zig cc -O1 -Wall -Werror "$out/link_test.c" -o "$out/link_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'C compilation failed' }
& "$out/link_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Link regression failed' }
