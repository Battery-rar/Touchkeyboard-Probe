param(
    [Parameter(Mandatory)] [string] $Zig,
    [string] $OutputDirectory,
    [switch] $Trace,
    [string] $NuGetPackages = "$env:USERPROFILE/.nuget/packages",
    [string] $WindowsSdk = "${env:ProgramFiles(x86)}\Windows Kits\10",
    [ValidateSet('x64', 'arm64')] [string] $Architecture = 'arm64',
    [string] $CppWinRT
)

$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$root = Split-Path $source
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'obj/zig-cache'
$Architecture = $Architecture.ToLowerInvariant()
$target = if ($Architecture -eq 'x64') { 'x86_64-windows-gnu' } else { 'aarch64-windows-gnu' }
$scratch = Join-Path $root "obj/native-$Architecture"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $scratch 'cache'
$output = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $root "bin/native-$Architecture" }
New-Item -ItemType Directory -Force $scratch, $output | Out-Null
$resource = Join-Path $scratch 'Keyboard.res'
$traceFlags = @(if ($Trace) { '-DFOCUS_TRACE' })
$sdkInclude = Join-Path $WindowsSdk 'Include/10.0.26100.0'
$winrt = Join-Path $WindowsSdk "Lib/10.0.26100.0/um/$Architecture/windowsapp.lib"
$projection = Join-Path $scratch 'projection'
if (!$CppWinRT) {
    $hostArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString().ToLowerInvariant()
    $CppWinRT = Join-Path $WindowsSdk "bin/10.0.26100.0/$hostArchitecture/cppwinrt.exe"
}
$metadata = Get-ChildItem -Recurse -File -Filter '*.winmd' -LiteralPath @(
    (Join-Path $NuGetPackages 'microsoft.windowsappsdk.winui/2.3.9/metadata')
    (Join-Path $NuGetPackages 'microsoft.windowsappsdk.interactiveexperiences/2.1.9/metadata/10.0.18362.0')
    (Join-Path $NuGetPackages 'microsoft.windowsappsdk.foundation/2.3.12/metadata')
    (Join-Path $NuGetPackages 'microsoft.web.webview2/1.0.3719.77/lib')
)
$references = Get-ChildItem (Join-Path $WindowsSdk 'References/10.0.26100.0') -Recurse -File -Filter '*.winmd'
$response = Join-Path $scratch 'winui.rsp'
$responseLines = @(
    '-input'
    $metadata | ForEach-Object { '"' + $_.FullName + '"' }
    $references | ForEach-Object { '"' + $_.FullName + '"' }
    '-output'
    '"' + $projection + '"'
)
[IO.File]::WriteAllLines($response, [string[]]$responseLines, [Text.UTF8Encoding]::new($false))
& $CppWinRT "@$response"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $Zig rc "/I$source" "/fo$resource" (Join-Path $source 'Keyboard.rc')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $Zig c++ -target $target -std=c++20 -Os -s -DUNICODE -D_UNICODE -DNOMINMAX `
    @traceFlags `
    -DNTDDI_VERSION=0x0A000007 -Wno-nonportable-include-path -isystem $projection `
    -idirafter (Join-Path $sdkInclude 'um') `
    -municode '-Wl,/subsystem:windows' (Join-Path $source 'Tray.cpp') (Join-Path $source 'TouchInput.cpp') `
    (Join-Path $source 'Focus.cpp') (Join-Path $source 'SystemMenu.cpp') `
    $resource -luser32 -ladvapi32 -lhid $winrt `
    -o (Join-Path $output "TouchKeyboardProbe.exe")
exit $LASTEXITCODE
