# DiskCleaner CLI smoke test: exercises every automated entry point.
# Usage: pwsh -NoProfile -File tools\smoke.ps1

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\DiskCleaner.exe'
$tmp = Join-Path $root 'work\smoke'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:PATH = "C:\Qt\6.11.2\mingw_64\bin;$env:PATH"

$script:fail = 0
$script:pass = 0

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) {
        $script:pass++
        Write-Host ("PASS  " + $name)
    } else {
        $script:fail++
        Write-Host ("FAIL  " + $name + "  " + $detail)
    }
}

function Run([string[]]$arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -Wait -NoNewWindow
    return $p.ExitCode
}

# 1) NTFS record parser self-test
$out = & $exe --selftest 2>&1 | Out-String
Check 'selftest' (($LASTEXITCODE -eq 0) -and ($out -match 'selftest PASS')) $out.Trim()

# 2) folder scan + CSV export
$csv = Join-Path $tmp 'export.csv'
Remove-Item $csv -ErrorAction SilentlyContinue
$code = Run @('--scan', (Join-Path $root 'work\sample'), '--export', $csv,
              '--shot', (Join-Path $tmp 'scan.png'), '--delay', '2000')
$head = if (Test-Path $csv) { (Get-Content $csv -TotalCount 1) } else { '' }
Check 'export csv' (($code -eq 0) -and $head.StartsWith('File Name,Path,Size,Allocated')) $head

# 3) CSV re-import round trip
$out = & $exe --scan-csv $csv 2>&1 | Out-String
Check 'import csv round-trip' (($LASTEXITCODE -eq 0) -and ($out -match 'size 5032') -and
                               ($out -match 'files 5')) $out.Trim()

# 4) full-column export
$full = Join-Path $tmp 'full.csv'
Remove-Item $full -ErrorAction SilentlyContinue
$code = Run @('--scan', (Join-Path $root 'work\sample'), '--export-full', $full,
              '--shot', (Join-Path $tmp 'full.png'), '--delay', '2000')
$head = if (Test-Path $full) { (Get-Content $full -TotalCount 1) } else { '' }
Check 'export full columns' (($code -eq 0) -and ($head -match 'MFTRECNO') -and
                             ($head -match 'PERCENTOFPARENT') -and ($head -match 'DRIVECAPACITY')) $head

# 5) synthetic MFT dump + load
$mft = Join-Path $tmp 'synthetic.mft'
Remove-Item $mft -ErrorAction SilentlyContinue
$code = Run @('--make-mft', $mft, '2000')
$out = & $exe --scan-mft $mft 2>&1 | Out-String
Check 'mft dump build+load' (($code -eq 0) -and ($out -match 'nodes= 2006') -and
                              ($out -match 'records= 2006')) $out.Trim()

# 6) multi-root merge
$out = & $exe --scan-multi ((Join-Path $root 'work\sample') + ';' + (Join-Path $root 'work\dups')) 2>&1 | Out-String
Check 'multi-root merge' (($LASTEXITCODE -eq 0) -and ($out -match 'parts= 2') -and
                          ($out -match 'files 10')) $out.Trim()

# 7) duplicate detection + filter + themes + language (UI paths)
$code = Run @('--scan', (Join-Path $root 'work\dups'), '--dup', '1', '--dup-only', '--tab', '1',
              '--shot', (Join-Path $tmp 'dups.png'), '--delay', '2000')
Check 'duplicates view' ($code -eq 0)

$code = Run @('--scan', (Join-Path $root 'work\sample'), '--filter', 'txt',
              '--shot', (Join-Path $tmp 'filter.png'), '--delay', '2000')
Check 'filter applied' ($code -eq 0)

$code = Run @('--theme', 'dark', '--lang', 'zh_CN', '--scan', (Join-Path $root 'work\sample'),
              '--shot', (Join-Path $tmp 'dark.png'), '--delay', '2000')
Check 'dark theme + chinese' (($code -eq 0) -and (Test-Path (Join-Path $tmp 'dark.png')))
$code = Run @('--theme', 'light', '--lang', 'en', '--scan', (Join-Path $root 'work\sample'),
              '--shot', (Join-Path $tmp 'light.png'), '--delay', '2000')
Check 'light theme + english' ($code -eq 0)

# 8) update check against a local endpoint
$http = Join-Path $tmp 'http'
New-Item -ItemType Directory -Force -Path $http | Out-Null
"VERSION|9.99`nINFOURL|https://example.com/releases" | Set-Content -Path (Join-Path $http 'ver.txt') -Encoding ASCII
$server = Start-Process -FilePath 'C:\Python\Python314\python.exe' `
    -ArgumentList '-m', 'http.server', '8137', '--bind', '127.0.0.1', '--directory', $http `
    -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$out = & $exe --check-update 'http://127.0.0.1:8137/ver.txt' 2>&1 | Out-String
Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
Check 'update check' (($LASTEXITCODE -eq 0) -and ($out -match 'newer= true') -and
                      ($out -match 'version= "9.99"')) $out.Trim()

Write-Host ""
Write-Host ("SMOKE RESULT: " + $script:pass + " passed, " + $script:fail + " failed")
exit $(if ($script:fail -eq 0) { 0 } else { 1 })
