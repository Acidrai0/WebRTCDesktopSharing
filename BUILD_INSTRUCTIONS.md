# Building the WebRTC Desktop Sharing Project

This document explains how to build the simplified WebRTC Desktop Sharing application.

## Prerequisites

- MinGW-w64 (for Windows)
- CMake 3.15 or higher
- Go 1.20 or higher (for the signaling server)

## Project Structure

The project is organized into three main components:

1. **C++ Desktop Client** - Captures the screen and encodes it
2. **Go Signaling Server** - Handles WebRTC signaling
3. **Web Frontend** - Displays the shared screen

## Building the Desktop Client

### Using PowerShell Script (Recommended)

The easiest way to build the project is using the included PowerShell script:

```powershell
# Run the build script
.\build.ps1
```

### Manual Build

To build manually:

```bash
# Create build directory
mkdir build
cd build

# Configure with CMake
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Build
mingw32-make

# The executable will be generated in the client/bin directory
```

## Running the Go Signaling Server

```bash
# Navigate to the server directory
cd server

# Download Go dependencies
go mod download

# Run the server (listening on port 8080 by default)
go run main.go
```

## Running the Web Client

Open `web/index.html` in a web browser after starting the signaling server.

## Development Roadmap

This project is currently in skeleton form. The following components need to be implemented:

1. **Screen Capture** - DXGI implementation is partially done, GDI fallback is stub
2. **Video Encoding** - Currently stub, will be implemented with x264
3. **WebRTC Integration** - Currently stub, will be implemented with a WebRTC library

## External Dependencies (To Be Added Later)

The following dependencies will be added with proper static linking:

- **x264** - For H.264 video encoding
- **libyuv** - For YUV conversion
- **WebRTC Native C++ SDK** - For peer connections 