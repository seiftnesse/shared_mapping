# Extract Windows kernel structure offsets from the msdl
#
# Pipeline:
#   ntoskrnl.exe --(llvm-readobj)--> CodeView RSDS {GUID, age, pdb name}
#              --(Invoke-WebRequest)--> PDB from msdl.microsoft.com
#              --(llvm-pdbutil)--> type layouts -> kernel_offsets.csv row.
#
# Usage:
#   .\dump-offsets.ps1                                   # host ntoskrnl
#   .\dump-offsets.ps1 -Image C:\vm\ntoskrnl.exe         # external path
#   .\dump-offsets.ps1 -Image <win11 ntoskrnl> -SaveLayouts
#
# The emitted CSV row appends to
# SharedMapping/SourceFiles/common/kernel_offsets.csv (columns after
# nt_version map 1:1 onto SM_KERNEL_OFFSETS, see common/kernel_offsets.h).

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
$version = (Get-Item $Image).VersionInfo.FileVersion
Write-Host "==> version: $version"
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

function Get-ClassLayout([string]$TypeName) {
    $dump = & $pdbutil pretty --classes --class-definitions=layout `
        --include-types="^$TypeName`$" $pdb 2>$null
    if ($SaveLayouts) {
        $dump | Add-Content -Path $layoutDump -Encoding utf8
    }
    return @($dump)
}

# Field offset inside a class layout dump: "data +0xNN [sizeof=..] Type Name".
# Matching runs in this scope (a Where-Object scriptblock would hide $Matches).
function Get-FieldOffset([string[]]$Layout, [string]$TypeName, [string]$Field) {
    foreach ($line in $Layout) {
        if ($line -match "data \+0x([0-9A-Fa-f]+).*\b$Field\b") {
            return [Convert]::ToInt64($Matches[1], 16)
        }
    }
    Write-Error "$TypeName.$Field not found in the PDB layout"
}

# Struct size from the header line: "struct _X [sizeof = N] {"
function Get-StructSize([string[]]$Layout, [string]$TypeName) {
    $line = $Layout | Where-Object { $_ -match "struct $TypeName \[sizeof = (\d+)\]" } |
        Select-Object -First 1
    if (-not $line) {
        Write-Error "$TypeName size not found in the PDB layout"
    }
    return [Convert]::ToInt64($Matches[1], 10)
}

$kprocess = Get-ClassLayout "_KPROCESS"
$mmpfn = Get-ClassLayout "_MMPFN"
$eprocess = Get-ClassLayout "_EPROCESS"
$mmFull = Get-ClassLayout "_MMSUPPORT_FULL"
$mmShared = Get-ClassLayout "_MMSUPPORT_SHARED"
foreach ($type in @("_MMPTE", "_MMPTE_HARDWARE")) {
    Get-ClassLayout $type | Out-Null  # documented layouts, kept for context
}

$dtbOff = Get-FieldOffset $kprocess "_KPROCESS" "DirectoryTableBase"
$udtbOff = Get-FieldOffset $kprocess "_KPROCESS" "UserDirectoryTableBase"
$kprocSize = Get-StructSize $kprocess "_KPROCESS"
$pfnElem = Get-StructSize $mmpfn "_MMPFN"
$pfnShareOff = Get-FieldOffset $mmpfn "_MMPFN" "u2"
$vmOff = Get-FieldOffset $eprocess "_EPROCESS" "Vm"
$mmSharedOff = Get-FieldOffset $mmFull "_MMSUPPORT_FULL" "Shared"
$shadowOff = Get-FieldOffset $mmShared "_MMSUPPORT_SHARED" "ShadowMapping"

# MmPfnDatabase pointer variable RVA: publics record "addr = SEGM:OFF",
# both DECIMAL (calibrated on MiCheckProcessShadow: seg 8 + off 63552 ==
# RVA 0x20F840); SEGM is the COFF section Number from llvm-readobj.
$pubs = & $pdbutil dump --publics $pdb 2>$null | Out-String
if ($pubs -notmatch '`MmPfnDatabase`[\s\S]*?addr = (\d+):(\d+)') {
    Write-Error "MmPfnDatabase public not found in the PDB"
}
$segment = [int]$Matches[1]
$segOff = [int64]$Matches[2]

$secDump = & $readobj --sections $Image | Out-String
$sections = @()
$cur = $null
foreach ($line in ($secDump -split "`n")) {
    if ($line -match 'Number:\s*(\d+)') { $cur = @{ num = [int]$Matches[1] } }
    elseif ($cur -and $line -match 'VirtualAddress:\s*0x([0-9A-Fa-f]+)') { $cur.rva = [Convert]::ToInt64($Matches[1], 16); $sections += $cur; $cur = $null }
}
$sec = $sections | Where-Object { $_.num -eq $segment } | Select-Object -First 1
if (-not $sec) {
    Write-Error "MmPfnDatabase segment $segment not found in the image sections"
}
$pfndbRva = $sec.rva + $segOff

# pfn_share_shift: semantic constant verified per-build against
# MiLockAndIncrementShareCount in IDA (0 => ShareCount occupies bits 61:0);
# every 19041/Win11 check so far reads 0. Re-verify on a new build before
# trusting the row.
$shareShift = 0

if ($version -notmatch '(\d+)\.(\d+)\.(\d+)\.(\d+)') {
    Write-Error "cannot parse FileVersion '$version'"
}
$imageBuild = $Matches[3]
$ubr = $Matches[4]

Write-Host "`n==> extracted offsets:" -ForegroundColor Cyan
Write-Host "    _KPROCESS.DirectoryTableBase     +0x$($dtbOff.ToString('x'))"
Write-Host "    _KPROCESS.UserDirectoryTableBase +0x$($udtbOff.ToString('x'))"
Write-Host "    sizeof(_KPROCESS)                0x$($kprocSize.ToString('x'))"
Write-Host "    sizeof(_MMPFN)                   0x$($pfnElem.ToString('x'))"
Write-Host "    _MMPFN.u2 (share count)          +0x$($pfnShareOff.ToString('x'))"
Write-Host "    _EPROCESS.Vm                     +0x$($vmOff.ToString('x'))"
Write-Host "    _MMSUPPORT_FULL.Shared           +0x$($mmSharedOff.ToString('x'))"
Write-Host "    _MMSUPPORT_SHARED.ShadowMapping  +0x$($shadowOff.ToString('x'))"
Write-Host "    MmPfnDatabase RVA                0x$($pfndbRva.ToString('x')) (seg $segment off $segOff)"

# os_build is the TARGET VM's RtlGetVersion build number (registry), not
# the image build: Win10 22H2 runs the 19041 image with os_build 19045.
$row = "<os_build>,$ubr,$imageBuild.$ubr,0x$($dtbOff.ToString('x')),0x$($udtbOff.ToString('x')),0x$($kprocSize.ToString('x')),0x$($pfnElem.ToString('x')),0x$($pfnShareOff.ToString('x')),$shareShift,0x$($pfndbRva.ToString('x')),0x$($vmOff.ToString('x')),0x$($mmSharedOff.ToString('x')),0x$($shadowOff.ToString('x'))"
Write-Host "    image build $imageBuild; replace <os_build> with the VM's" -ForegroundColor Yellow
Write-Host "    RtlGetVersion build (19045 on a 22H2 install with this image)" -ForegroundColor Yellow
Write-Host "`n==> append to SharedMapping/SourceFiles/common/kernel_offsets.csv:" -ForegroundColor Yellow
Write-Host $row
Write-Host "`n    then verify the pfn_share_shift against MiLockAndIncrementShareCount" -ForegroundColor Yellow
Write-Host "    in IDA, and rebuild + ctest before deploying." -ForegroundColor Yellow
