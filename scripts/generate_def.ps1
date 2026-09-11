# Pins a proxy's exports to whatever ordinal the real DLL on this machine
# uses: some importers (e.g. NVIDIA Streamline's NRI.dll, against d3d12.dll)
# bind by ordinal, not name, and ordinals aren't guaranteed stable across
# Windows versions. Only the handful of commonly-used exports are
# implemented. The rest are left unexported, which fails safely if
# anything calls GetProcAddress for one.
param(
    [Parameter(Mandatory=$true)][string]$DumpbinPath,
    [Parameter(Mandatory=$true)][string]$OutFile,
    [Parameter(Mandatory=$true)][string]$RealDllName,   # e.g. "d3d12.dll"
    [Parameter(Mandatory=$true)][string]$LibraryName,   # e.g. "d3d12", must match OUTPUT_NAME
    [Parameter(Mandatory=$true)][string]$Exports        # comma-separated, since CMake's execute_process
                                                          # passes each COMMAND token as its own argv
                                                          # entry, so a real string[] never reassembles
                                                          # correctly across that boundary.
)

# A NEW variable, not a reassignment of $Exports: PowerShell re-coerces any
# value assigned back into a [type]-constrained parameter to its declared
# type, so "$Exports = $Exports -split ','" would silently rejoin the split
# array into a single string instead of keeping it as an array.
$exportNames = $Exports -split ','

$real = Join-Path $env:WINDIR "System32\$RealDllName"
if (-not (Test-Path $real)) {
    throw "Real $RealDllName not found at $real"
}

$output = & $DumpbinPath "/EXPORTS" $real
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed against $real"
}

# ordinal, hint, RVA, name, with an optional " = target" suffix, which is how
# dumpbin renders a forwarded export. A forwarder still occupies a real ordinal,
# which is exactly what has to be pinned, so it must not be skipped: without the
# suffix the line wouldn't match at all and the export would be reported missing.
$pattern = '^\s*(\d+)\s+[0-9A-Fa-f]*\s+\S+\s+(\S+)(\s*=.*)?\s*$'

$ordinals = @{}
foreach ($line in $output) {
    if ($line -match $pattern -and $exportNames -contains $Matches[2]) {
        $ordinals[$Matches[2]] = [int]$Matches[1]
    }
}

$missing = $exportNames | Where-Object { -not $ordinals.ContainsKey($_) }
if ($missing) {
    throw "Not found in $RealDllName's export table: $($missing -join ', ')"
}

$lines = @("LIBRARY $LibraryName", "EXPORTS")
foreach ($name in $exportNames) {
    $lines += "    $name @$($ordinals[$name])"
}

Set-Content -Path $OutFile -Value $lines -Encoding ASCII
Write-Host "Generated $OutFile with $($exportNames.Count) exports pinned to their real ordinals (source: $real)"
