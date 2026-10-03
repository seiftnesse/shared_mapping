# Extract Windows kernel structure offsets from the msdl
#
# Pipeline:
#   ntoskrnl.exe --(llvm-readobj)--> CodeView RSDS {GUID, age, pdb name}
#              --(Invoke-WebRequest)--> PDB from msdl.microsoft.com
#              --(llvm-pdbutil)--> type layouts.
#
# Usage:
#   .\dump-offsets.ps1                                   # host ntoskrnl
#   .\dump-offsets.ps1 -Image C:\vm\ntoskrnl.exe         # external path

param(
    [string]$Image = "C:\Windows\System32\ntoskrnl.exe",
    [string]$SymbolDir = "$PSScriptRoot\..\build\symbols",
    [string]$SymbolServer = "https://msdl.microsoft.com/download/symbols",

    [switch]$SaveLayouts
)

$ErrorActionPreference = "Stop"
$SymbolDir = [IO.Path]::GetFullPath($SymbolDir)

$readobj = (Get-Command llvm-readobj -ErrorAction Stop).Source
$pdbutil = (Get-Command llvm-pdbutil -ErrorAction Stop).Source

# CodeView RSDS info: {GUID, age, pdb name}
$debugDir = & $readobj --coff-debug-directory $Image | Out-String
if ($debugDir -notmatch 'PDBSignature:\s*0x53445352') {
    Write-Error "no RSDS CodeView record in $Image"
}
$debugDir -match 'PDBGUID:\s*\{([0-9A-Fa-f-]+)\}' | Out-Null
$guid = $Matches[1]
$debugDir -match 'PDBAge:\s*(\d+)' | Out-Null
$age = $Matches[1]
$debugDir -match 'PDBFileName:\s*(\S+\.pdb)' | Out-Null
$pdbName = $Matches[1]

$guidCompact = $guid -replace '-', ''
$storeDir = Join-Path $SymbolDir "$pdbName\$guidCompact$age"
$pdb = Join-Path $storeDir $pdbName

Write-Host "==> image:   $Image"
Write-Host "==> version: $((Get-Item $Image).VersionInfo.FileVersion)"
Write-Host "==> symbols: $pdbName GUID {$guid} age $age"

if (-not (Test-Path $pdb)) {
    New-Item -ItemType Directory -Force -Path $storeDir | Out-Null
    $uri = "$SymbolServer/$pdbName/$guidCompact$age/$pdbName"
    Write-Host "==> downloading $uri"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $uri -OutFile $pdb -UserAgent "Microsoft-Symbol-Server"
} else {
    Write-Host "==> using cached PDB"
}

$layoutDump = if ($SaveLayouts) {
    Join-Path $storeDir "$([IO.Path]::GetFileNameWithoutExtension($pdbName)).layouts.txt"
}
foreach ($type in @("_KPROCESS", "_MMPTE", "_MMPTE_HARDWARE", "_MMPFN")) {
    Write-Host "`n===== $type =====" -ForegroundColor Cyan
    $dump = & $pdbutil pretty --classes --class-definitions=layout `
        --include-types="^$type`$" $pdb 2>$null
    $dump | Where-Object { $_ -match "struct $type|union $type|\+0x" }
    if ($SaveLayouts) {
        $dump | Add-Content -Path $layoutDump -Encoding utf8
    }
}
if ($SaveLayouts) {
    Write-Host "`n==> full layouts saved: $layoutDump" -ForegroundColor Yellow
}
