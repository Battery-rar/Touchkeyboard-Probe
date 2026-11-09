param(
    [Parameter(Mandatory)] [string] $Zig,
    [string] $OutputDirectory,
    [switch] $Trace,
    [string] $NuGetPackages = "$env:USERPROFILE/.nuget/packages",
    [string] $WindowsSdk = "${env:ProgramFiles(x86)}\Windows Kits\10"
)

$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$root = Split-Path $source
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'obj/zig-cache'
$scratch = Join-Path $root 'obj/native-arm64'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $scratch 'cache'
$output = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $root 'bin/native-arm64' }
New-Item -ItemType Directory -Force $scratch, $output | Out-Null
$resource = Join-Path $scratch 'Keyboard.res'
$traceFlags = @(if ($Trace) { '-DFOCUS_TRACE' })
$sdkInclude = Join-Path $WindowsSdk 'Include/10.0.26100.0'
$winrt = Join-Path $WindowsSdk 'Lib/10.0.26100.0/um/arm64/windowsapp.lib'
$projection = Join-Path $scratch 'winui'
$metadata = Get-ChildItem -Recurse -File -Filter '*.winmd' -LiteralPath @(
    (Join-Path $NuGetPackages 'microsoft.windowsappsdk.winui/2.3.9/metadata')
    (Join-Path $NuGetPackages 'microsoft.windowsappsdk.interactiveexperiences/2.1.9/metadata/10.0.18362.0')
    (Join-Path $NuGetPackages 'microsoft.windowsappsdk.foundation/2.3.12/metadata')
    (Join-Path $NuGetPackages 'microsoft.web.webview2/1.0.3719.77/lib')
)
$references = Get-ChildItem (Join-Path $WindowsSdk 'References/10.0.26100.0') -Recurse -File -Filter '*.winmd'
$response = Join-Path $scratch 'winui.rsp'
@(
    '-input'
    $metadata | ForEach-Object { '"' + $_.FullName + '"' }
    '-reference'
    $references | ForEach-Object { '"' + $_.FullName + '"' }
    '-output'
    '"' + $projection + '"'
) | Set-Content $response
& (Join-Path $WindowsSdk 'bin/10.0.26100.0/arm64/cppwinrt.exe') "@$response"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $Zig rc "/I$source" "/fo$resource" (Join-Path $source 'Keyboard.rc')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $Zig c++ -target aarch64-windows-gnu -std=c++20 -Os -s -DUNICODE -D_UNICODE -DNOMINMAX `
    @traceFlags `
    -DNTDDI_VERSION=0x0A000007 -Wno-nonportable-include-path -isystem $projection -isystem (Join-Path $sdkInclude 'cppwinrt') `
    -idirafter (Join-Path $sdkInclude 'um') `
    -municode '-Wl,/subsystem:windows' (Join-Path $source 'Tray.cpp') (Join-Path $source 'TouchInput.cpp') `
    (Join-Path $source 'Focus.cpp') (Join-Path $source 'SystemMenu.cpp') `
    $resource -luser32 -ladvapi32 -lhid $winrt `
    -o (Join-Path $output 'TouchKeyboardProbe.exe')
exit $LASTEXITCODE
