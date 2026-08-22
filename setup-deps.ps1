$ErrorActionPreference = 'Stop'

$Organization = "moonlight-stream"
$PrebuiltRepo = "moonlight-qt-deps"
$TargetDir = Join-Path $PSScriptRoot "libs\windows"
$Assets = @("windows-x64.zip", "windows-ARM64.zip")
$Tag = "v12"
$MsQuicVersion = "2.5.10"
$MsQuicSha256 = "893b1be02f2d965a24ce319b4acd6ff95294d33c819614c4125cdcf83bd33cd5"

if (Test-Path $TargetDir) {
    Write-Host "Cleaning target directory..." -ForegroundColor Cyan
    Remove-Item -Path "$TargetDir\*" -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $TargetDir | Out-Null
}

foreach ($AssetName in $Assets) {
    $Url = "https://github.com/$Organization/$PrebuiltRepo/releases/download/$Tag/$AssetName"
    $ArchivePath = Join-Path $env:TEMP $AssetName

    Write-Host "Downloading $AssetName..." -ForegroundColor Cyan
    curl.exe -s -L -f -o "$ArchivePath" "$Url"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    Write-Host "Extracting $AssetName..." -ForegroundColor Cyan
    Expand-Archive -Path $ArchivePath -DestinationPath $TargetDir -Force
    Remove-Item $ArchivePath
}

# MsQuic is kept separate from the Moonlight dependency archive so its
# security updates can be pinned and reviewed independently.
$MsQuicArchive = Join-Path $env:TEMP "msquic-$MsQuicVersion.zip"
$MsQuicExtract = Join-Path $env:TEMP "msquic-$MsQuicVersion-$PID"
$MsQuicUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.native.quic.msquic.openssl/$MsQuicVersion/microsoft.native.quic.msquic.openssl.$MsQuicVersion.nupkg"
Write-Host "Downloading MsQuic $MsQuicVersion..." -ForegroundColor Cyan
curl.exe -s -L -f -o "$MsQuicArchive" "$MsQuicUrl"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
if ((Get-FileHash -Algorithm SHA256 $MsQuicArchive).Hash.ToLowerInvariant() -ne $MsQuicSha256) {
    throw "MsQuic package checksum mismatch"
}
Expand-Archive -Path $MsQuicArchive -DestinationPath $MsQuicExtract
foreach ($Architecture in @("x64", "arm64")) {
    $LibraryDir = Join-Path $TargetDir "lib\$Architecture"
    New-Item -ItemType Directory -Path $LibraryDir -Force | Out-Null
    Copy-Item (Join-Path $MsQuicExtract "build\native\lib\$Architecture\msquic.lib") $LibraryDir -Force
    Copy-Item (Join-Path $MsQuicExtract "build\native\bin\$Architecture\msquic.dll") $LibraryDir -Force
    Copy-Item (Join-Path $MsQuicExtract "build\native\bin\$Architecture\msquic.pdb") $LibraryDir -Force
}
Remove-Item $MsQuicArchive
Remove-Item $MsQuicExtract -Recurse

Write-Host "Dependencies successfully deployed" -ForegroundColor Green
