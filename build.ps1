# WebRTCDesktopSharing Build Script

# Function to check if a command exists
function Test-Command {
    param([string]$Command)
    return (Get-Command $Command -ErrorAction SilentlyContinue) -ne $null
}

# Check for required tools
if (-not (Test-Command "cmake")) {
    Write-Error "CMake is not installed or not in PATH. Please install CMake."
    exit 1
}

if (-not (Test-Command "mingw32-make")) {
    Write-Error "MinGW-w64 is not installed or not in PATH. Please install MinGW-w64."
    exit 1
}

# Create build directory
$BuildDir = Join-Path $PSScriptRoot "build"
if (-not (Test-Path $BuildDir)) {
    Write-Host "Creating build directory..."
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Change to build directory
Push-Location $BuildDir

# Configure with CMake
Write-Host "Configuring project with CMake..."
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Check if CMake succeeded
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed."
    Pop-Location
    exit 1
}

# Build with MinGW
Write-Host "Building project..."
mingw32-make -j4

# Check if build succeeded
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed."
    Pop-Location
    exit 1
}

# Return to original directory
Pop-Location

Write-Host "Build completed successfully!"
Write-Host "The executable can be found at client/bin/desktop_sharing_client.exe" 