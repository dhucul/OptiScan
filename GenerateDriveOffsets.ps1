$url = "http://www.accuraterip.com/driveoffsets.htm"
$html = (Invoke-WebRequest -Uri $url -UseBasicParsing).Content

$entries = @()
$seen = @{}
foreach ($row in [regex]::Matches($html, '<tr[^>]*>(.*?)</tr>', 'Singleline,IgnoreCase')) {
    $cells = [regex]::Matches($row.Groups[1].Value, '<td[^>]*>(.*?)</td>', 'Singleline,IgnoreCase')
    if ($cells.Count -lt 3) { continue }

    $driveName = ($cells[0].Groups[1].Value -replace '<[^>]+>','').Trim()
    $offsetStr = ($cells[1].Groups[1].Value -replace '<[^>]+>','').Trim()

    if ($offsetStr -match 'Purged|Offset') { continue }
    $cleaned = ($offsetStr -replace '[^0-9\+\-]','')
    if (-not $cleaned) { continue }
    $offset = [int]$cleaned

    $dashIdx = $driveName.IndexOf(' - ')
    if ($dashIdx -ge 0) {
        $vendor = $driveName.Substring(0, $dashIdx).Trim()
        $model  = $driveName.Substring($dashIdx + 3).Trim()
    } else {
        $vendor = ""
        $model  = $driveName.Trim()
    }

    # Strip leading "- " from model (bad split artifacts)
    $model = $model -replace '^-\s+', ''

    if (-not $model) { continue }
    # Skip entries where model is too short / generic (false-positive risk)
    if ($model.Length -lt 4) { continue }

    $vendor = $vendor -replace '\\','\\\\'  -replace '"','\"'
    $model  = $model  -replace '\\','\\\\'  -replace '"','\"'

    # Deduplicate by vendor+model
    $key = "$vendor|$model"
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true

    $entries += [pscustomobject]@{ V=$vendor; M=$model; O=$offset }
}

$out = @"
// ============================================================================
// DriveOffsets.h - AccurateRip drive offset database
// AUTO-GENERATED from http://www.accuraterip.com/driveoffsets.htm
// Total entries: $($entries.Count)
// Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
// ============================================================================
#pragma once

struct DriveOffsetEntry {
    const char* vendor;
    const char* model;
    int offset;
};

static const DriveOffsetEntry knownOffsets[] = {
"@

foreach ($e in $entries) {
    $os = if ($e.O -ge 0) { "+$($e.O)" } else { "$($e.O)" }
    $out += "    {`"$($e.V)`", `"$($e.M)`", $os},`n"
}
$out += "    {nullptr, nullptr, 0}`n};`n"

[System.IO.File]::WriteAllText("DriveOffsets.h", $out, [System.Text.UTF8Encoding]::new($false))
Write-Host "Generated DriveOffsets.h with $($entries.Count) entries"