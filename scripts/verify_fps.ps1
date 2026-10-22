# High Snipping Frames - Video Verification Script

Write-Host "Searching for latest Snipping Tool video recording..." -ForegroundColor Cyan

$searchDirs = @(
    (Join-Path $env:USERPROFILE "Videos\Screen Recordings"),
    (Join-Path $env:LOCALAPPDATA "Packages\Microsoft.ScreenSketch_8wekyb3d8bbwe\TempState\Recordings")
)

$latest = Get-ChildItem -Path $searchDirs -Filter "*.mp4" -ErrorAction SilentlyContinue | 
          Sort-Object LastWriteTime -Descending | 
          Select-Object -First 1

if (-not $latest) {
    Write-Host "No screen recordings found. Please record a video with Snipping Tool first." -ForegroundColor Yellow
    exit 0
}

Write-Host "Latest Recording: $($latest.FullName)" -ForegroundColor Green
Write-Host "File Size: $([math]::Round($latest.Length / 1MB, 2)) MB" -ForegroundColor Gray
Write-Host "Recorded:  $($latest.LastWriteTime)" -ForegroundColor Gray
Write-Host "`nRunning ffprobe frame analysis..." -ForegroundColor Cyan

$probe = ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate,avg_frame_rate,nb_frames,duration,width,height -of default=noprint_wrappers=1:nokey=0 "$($latest.FullName)"

$probe | ForEach-Object {
    Write-Host "  $_" -ForegroundColor White
}
