# Fails if an MSBuild log has compiler or linker warnings outside the fetched
# dependencies. Project code must build with zero warnings (CLAUDE.md section 9).
param([Parameter(Mandatory = $true)][string]$Log)

$hits = Select-String -Path $Log -Pattern '\bwarning [A-Z]+[0-9]+' |
    Where-Object { $_.Line -notmatch '[\\/]_deps[\\/]' } |
    ForEach-Object { $_.Line.Trim() } |
    Sort-Object -Unique

if ($hits) {
    $hits | ForEach-Object { Write-Output $_ }
    Write-Error "$(@($hits).Count) warning(s) in project code"
    exit 1
}
Write-Output "No warnings in project code."
