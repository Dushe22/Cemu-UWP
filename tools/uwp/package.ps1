$ErrorActionPreference = 'Stop'
$root   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out    = Join-Path $root 'out\package'
$layout = Join-Path $out 'layout'
$sdk    = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'

# layout mirrors the VS deployment rules in src/uwp/CMakeLists.txt
if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
New-Item -ItemType Directory -Force $layout | Out-Null
Copy-Item "$root\bin\uwp\*" $layout -Recurse
Copy-Item "$root\bin\gameProfiles" $layout -Recurse
Copy-Item "$root\bin\resources\*" "$layout\resources" -Recurse -Force
New-Item -ItemType Directory -Force "$layout\licenses\ffmpeg" | Out-Null
Copy-Item "$root\dependencies\uwp\ffmpeg\6.0\LICENSE.txt" "$layout\licenses\ffmpeg\"
# the Redist/D3D DXC (10.0.19041.x) has no IDxcCompiler3 -> E_NOINTERFACE and a black screen on the console
foreach ($dll in 'dxcompiler.dll', 'dxil.dll') {
    $p = Join-Path $layout $dll
    if (-not (Test-Path $p)) { throw "$dll missing from bin\uwp" }
    $v = (Get-Item $p).VersionInfo
    if ($v.FileMajorPart -ne 1 -or $v.FileMinorPart -lt 7) {
        throw "$dll is version $($v.FileVersion); expected the DXC 1.7+ from the SDK bin folder (rebuild after configure.bat)"
    }
    Write-Host "$dll $($v.FileVersion)"
}
(Get-Content "$root\src\uwp\Package.appxmanifest" -Raw).Replace('$targetnametoken$', 'CemuUwp') |
    Set-Content -Encoding utf8 "$layout\AppxManifest.xml"

$version = ([xml](Get-Content "$layout\AppxManifest.xml")).Package.Identity.Version
$msix = Join-Path $out "Cemu_${version}_x64.msix"
& "$sdk\makeappx.exe" pack /o /d $layout /p $msix
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }

# test certificate; subject must match the manifest Publisher
$pfx = Join-Path $out 'CemuUwp_test.pfx'
$cer = Join-Path $out 'CemuUwp_test.cer'
$pwd = ConvertTo-SecureString -String 'cemu' -Force -AsPlainText
if (-not (Test-Path $pfx)) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=DanP142' -KeyUsage DigitalSignature `
        -FriendlyName 'Cemu UWP test signing' -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pwd | Out-Null
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
}
& "$sdk\signtool.exe" sign /fd SHA256 /a /f $pfx /p cemu $msix
if ($LASTEXITCODE -ne 0) { throw "signtool failed" }

Copy-Item 'C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx' $out
Get-ChildItem $out -File | Select-Object Name, Length
