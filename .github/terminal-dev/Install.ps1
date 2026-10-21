# Installs the Terminal Dev package that sits next to this script.
# Trusts the self-signed signing certificate first (one admin prompt), then
# installs the MSIX plus any framework dependency that isn't already present.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-PackageIdentity([string]$Path) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $reader = [IO.StreamReader]::new($zip.GetEntry('AppxManifest.xml').Open())
        try { ([xml]$reader.ReadToEnd()).Package.Identity } finally { $reader.Dispose() }
    } finally {
        $zip.Dispose()
    }
}

$cer = Get-ChildItem $PSScriptRoot -Filter *.cer | Select-Object -First 1
$msix = Get-ChildItem $PSScriptRoot -Filter *.msix | Select-Object -First 1
if (-not $cer -or -not $msix) { throw "Expected a .cer and a .msix next to $PSCommandPath" }

$thumbprint = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cer.FullName).Thumbprint
function Test-Trusted { [bool](Get-ChildItem Cert:\LocalMachine\TrustedPeople | Where-Object Thumbprint -EQ $thumbprint) }
if (-not (Test-Trusted)) {
    Write-Host 'Trusting the signing certificate (admin prompt)...'
    $command = "Import-Certificate -FilePath '$($cer.FullName)' -CertStoreLocation Cert:\LocalMachine\TrustedPeople"
    Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile', '-Command', $command
    if (-not (Test-Trusted)) { throw 'The certificate was not trusted; the package cannot be installed.' }
}

$deps = foreach ($appx in Get-ChildItem (Join-Path $PSScriptRoot 'Dependencies') -Filter *.appx -ErrorAction SilentlyContinue) {
    $id = Get-PackageIdentity $appx.FullName
    $installed = Get-AppxPackage -Name $id.Name | Where-Object { "$($_.Architecture)" -eq $id.ProcessorArchitecture -and [version]$_.Version -ge [version]$id.Version }
    if (-not $installed) { $appx.FullName }
}

Write-Host "Installing $($msix.Name)..."
Add-AppxPackage -Path $msix.FullName -DependencyPath @($deps) -ForceUpdateFromAnyVersion
Get-AppxPackage WindowsTerminalDev | Select-Object Name, Version, PackageFamilyName
