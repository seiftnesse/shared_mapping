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

function Convert-Hex([long]$Value) { '0x{0:x}' -f $Value }

# -match and return the first capture group, or fail with context.
function Find-Capture([string]$Text, [string]$Pattern, [string]$What) {
    if ($Text -notmatch $Pattern) {
        Write-Error "cannot parse $What (expected /$Pattern/)"
    }
    return $Matches[1]
}

# --- CodeView RSDS: {GUID, age, pdb name} ---------------------------------

$debugDir = & $readobj --coff-debug-directory $Image | Out-String
if ($debugDir -notmatch 'PDBSignature:\s*0x53445352') {
    Write-Error "no RSDS CodeView record in $Image"
}
$guid = Find-Capture $debugDir 'PDBGUID:\s*\{([0-9A-Fa-f-]+)\}' 'PDBGUID'
$age = Find-Capture $debugDir 'PDBAge:\s*(\d+)' 'PDBAge'
$pdbName = Find-Capture $debugDir 'PDBFileName:\s*(\S+\.pdb)' 'PDBFileName'

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

# --- one pdbutil pass for every type we need ------------------------------
# A single pretty-print over a type alternation is far cheaper than one
# run per type; slices per class are cut on the top-level header lines
# ("struct _NAME [sizeof = N] {", never indented).

$layoutTypes = @('_KPROCESS', '_MMPFN', '_EPROCESS', '_MMSUPPORT_FULL',
    '_MMSUPPORT_SHARED')
if ($SaveLayouts) {
    # Documented PTE layouts, kept for human context only.
    $layoutTypes += @('_MMPTE', '_MMPTE_HARDWARE')
}
$includeRegex = '^(' + ($layoutTypes -join '|') + ')$'

$allLayouts = @(& $pdbutil pretty --classes --class-definitions=layout `
        --include-types=$includeRegex $pdb 2>$null)
if ($SaveLayouts) {
    $layoutDump = Join-Path $storeDir `
        "$([IO.Path]::GetFileNameWithoutExtension($pdbName)).layouts.txt"
    $allLayouts | Add-Content -Path $layoutDump -Encoding utf8
}

function Get-ClassLayout([string]$TypeName) {
    $slice = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in $allLayouts) {
        # Top-level class headers ("    struct _NAME [sizeof = N] {"):
        # nested members print as "data +0x.." lines, never as headers.
        if ($line -match '^\s*struct\s+(_\w+)\s*\[sizeof\s*=') {
            if ($inside) { break }  # the next top-level class: done
            $inside = ($Matches[1] -eq $TypeName)
        }
        if ($inside) { $slice.Add($line) }
    }
    if ($slice.Count -eq 0) {
        Write-Error "$TypeName not found in the PDB layout"
    }
    return $slice
}

# Field offset inside a class layout dump: "data +0xNN [sizeof=..] Type Name".
# Plain foreach on purpose: $Matches set inside a Where-Object scriptblock
# would not reach this scope.
function Get-FieldOffset([string[]]$Layout, [string]$TypeName, [string]$Field) {
    foreach ($line in $Layout) {
        if ($line -match "data \+0x([0-9A-Fa-f]+).*\b$Field\b") {
            return [Convert]::ToInt64($Matches[1], 16)
        }
    }
    Write-Error "$TypeName.$Field not found in the PDB layout"
}

# Struct size from the header line: "struct _X [sizeof = N] {".
function Get-StructSize([string[]]$Layout, [string]$TypeName) {
    foreach ($line in $Layout) {
        if ($line -match "struct $TypeName \[sizeof = (\d+)\]") {
            return [Convert]::ToInt64($Matches[1], 10)
        }
    }
    Write-Error "$TypeName size not found in the PDB layout"
}

$kprocess = Get-ClassLayout '_KPROCESS'
$mmpfn = Get-ClassLayout '_MMPFN'
$eprocess = Get-ClassLayout '_EPROCESS'
$mmFull = Get-ClassLayout '_MMSUPPORT_FULL'
$mmShared = Get-ClassLayout '_MMSUPPORT_SHARED'

$dtbOff = Get-FieldOffset $kprocess '_KPROCESS' 'DirectoryTableBase'
$udtbOff = Get-FieldOffset $kprocess '_KPROCESS' 'UserDirectoryTableBase'
$kprocSize = Get-StructSize $kprocess '_KPROCESS'
$pfnElem = Get-StructSize $mmpfn '_MMPFN'
$pfnShareOff = Get-FieldOffset $mmpfn '_MMPFN' 'u2'
$vmOff = Get-FieldOffset $eprocess '_EPROCESS' 'Vm'
$sbaOff = Get-FieldOffset $eprocess '_EPROCESS' 'SectionBaseAddress'
$pebOff = Get-FieldOffset $eprocess '_EPROCESS' 'Peb'
$mmSharedOff = Get-FieldOffset $mmFull '_MMSUPPORT_FULL' 'Shared'
$shadowOff = Get-FieldOffset $mmShared '_MMSUPPORT_SHARED' 'ShadowMapping'

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
Write-Host "    _KPROCESS.DirectoryTableBase     +$(Convert-Hex $dtbOff)"
Write-Host "    _KPROCESS.UserDirectoryTableBase +$(Convert-Hex $udtbOff)"
Write-Host "    sizeof(_KPROCESS)                $(Convert-Hex $kprocSize)"
Write-Host "    sizeof(_MMPFN)                   $(Convert-Hex $pfnElem)"
Write-Host "    _MMPFN.u2 (share count)          +$(Convert-Hex $pfnShareOff)"
Write-Host "    _EPROCESS.Vm                     +$(Convert-Hex $vmOff)"
Write-Host "    _MMSUPPORT_FULL.Shared           +$(Convert-Hex $mmSharedOff)"
Write-Host "    _MMSUPPORT_SHARED.ShadowMapping  +$(Convert-Hex $shadowOff)"
Write-Host "    _EPROCESS.SectionBaseAddress     +$(Convert-Hex $sbaOff)"
Write-Host "    _EPROCESS.Peb                    +$(Convert-Hex $pebOff)"
Write-Host "    MmPfnDatabase RVA                $(Convert-Hex $pfndbRva) (seg $segment off $segOff)"

# os_build is the TARGET VM's RtlGetVersion build number (registry), not
# the image build: Win10 22H2 runs the 19041 image with os_build 19045.
$row = (@(
        '<os_build>', $ubr, "$imageBuild.$ubr",
        (Convert-Hex $dtbOff), (Convert-Hex $udtbOff), (Convert-Hex $kprocSize),
        (Convert-Hex $pfnElem), (Convert-Hex $pfnShareOff), $shareShift,
        (Convert-Hex $pfndbRva), (Convert-Hex $vmOff), (Convert-Hex $mmSharedOff),
        (Convert-Hex $shadowOff), (Convert-Hex $sbaOff), (Convert-Hex $pebOff)
    ) -join ',')
Write-Host "    image build $imageBuild; replace <os_build> with the VM's" -ForegroundColor Yellow
Write-Host "    RtlGetVersion build (19045 on a 22H2 install with this image)" -ForegroundColor Yellow
Write-Host "`n==> append to SharedMapping/SourceFiles/common/kernel_offsets.csv:" -ForegroundColor Yellow
Write-Host $row
Write-Host "`n    then verify the pfn_share_shift against MiLockAndIncrementShareCount" -ForegroundColor Yellow
Write-Host "    in IDA, and rebuild + ctest before deploying." -ForegroundColor Yellow
