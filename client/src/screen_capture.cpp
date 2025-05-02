#include "screen_capture.h"

#include <iostream>
#include <memory>
#include <stdexcept>

ScreenCapture::ScreenCapture() {
    m_usingDXGI = true;
    m_captureCursor = true;
    m_cursorVisible = false;
    m_acquiredDesktopImage = nullptr;
    m_stagingTextures[0] = nullptr;
    m_stagingTextures[1] = nullptr;
    m_stagingTextures[2] = nullptr;
    m_currentTextureIndex = 0;
    m_frameCount = 0;
    m_framerate = 0.0f;
    m_bufferingMode = BufferingMode::Double; // Default to double buffering
    m_alignedBufferPadding = MEMORY_ALIGNMENT; // Set default padding for alignment
    
    // Initialize cursor position
    GetCursorPos(&m_cursorPosition);
    
    // Initialize performance counter for framerate calculation
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&m_lastCaptureTime);
    
    // Initialize cursor update time
    m_lastCursorUpdateTime = m_lastCaptureTime;
}

ScreenCapture::~ScreenCapture() {
    CleanupDXGI();
    CleanupGDI();
}

bool ScreenCapture::Initialize(int monitorIndex) {
    m_monitorIndex = monitorIndex;
    
    // Try DXGI first
    if (InitializeDXGI()) {
        std::cout << "Successfully initialized DXGI screen capture" << std::endl;
        m_usingDXGI = true;
        return true;
    }
    
    // Fall back to GDI
    std::cout << "DXGI initialization failed, falling back to GDI" << std::endl;
    if (FallbackToGDI()) {
        std::cout << "Successfully initialized GDI screen capture" << std::endl;
        m_usingDXGI = false;
        return true;
    }
    
    std::cerr << "Failed to initialize screen capture" << std::endl;
    return false;
}

bool ScreenCapture::InitializeDXGI() {
    try {
        // Create D3D11 device
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        UINT flags = 0;
        
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            &featureLevel,
            1,
            D3D11_SDK_VERSION,
            &m_d3dDevice,
            nullptr,
            &m_d3dContext
        );
        
        if (FAILED(hr)) {
            std::cerr << "Failed to create D3D11 device: " << std::hex << hr << std::endl;
            return false;
        }
        
        // Get DXGI device
        IDXGIDevice* dxgiDevice = nullptr;
        hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        
        if (FAILED(hr)) {
            std::cerr << "Failed to get DXGI device: " << std::hex << hr << std::endl;
            return false;
        }
        
        // Get DXGI adapter
        IDXGIAdapter* dxgiAdapter = nullptr;
        hr = dxgiDevice->GetAdapter(&dxgiAdapter);
        dxgiDevice->Release();
        
        if (FAILED(hr)) {
            std::cerr << "Failed to get DXGI adapter: " << std::hex << hr << std::endl;
            return false;
        }
        
        // Get output (monitor)
        IDXGIOutput* dxgiOutput = nullptr;
        hr = dxgiAdapter->EnumOutputs(m_monitorIndex, &dxgiOutput);
        dxgiAdapter->Release();
        
        if (FAILED(hr)) {
            std::cerr << "Failed to get DXGI output: " << std::hex << hr << std::endl;
            return false;
        }
        
        // Get output description (monitor info)
        DXGI_OUTPUT_DESC outputDesc;
        hr = dxgiOutput->GetDesc(&outputDesc);
        
        if (FAILED(hr)) {
            std::cerr << "Failed to get output description: " << std::hex << hr << std::endl;
            dxgiOutput->Release();
            return false;
        }
        
        // Store monitor dimensions and position
        m_monitorRect = outputDesc.DesktopCoordinates;
        m_monitorWidth = m_monitorRect.right - m_monitorRect.left;
        m_monitorHeight = m_monitorRect.bottom - m_monitorRect.top;
        
        // Get DXGI output 1
        IDXGIOutput1* dxgiOutput1 = nullptr;
        hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&dxgiOutput1));
        dxgiOutput->Release();
        
        if (FAILED(hr)) {
            std::cerr << "Failed to get DXGI output 1: " << std::hex << hr << std::endl;
            return false;
        }
        
        // Create desktop duplication
        hr = dxgiOutput1->DuplicateOutput(m_d3dDevice, &m_dxgiOutputDuplication);
        dxgiOutput1->Release();
        
        if (FAILED(hr)) {
            std::cerr << "Failed to create desktop duplication: " << std::hex << hr << std::endl;
            return false;
        }
        
        std::cout << "DXGI initialization successful for monitor " << m_monitorIndex 
                  << " (" << m_monitorWidth << "x" << m_monitorHeight << ")" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception during DXGI initialization: " << e.what() << std::endl;
        CleanupDXGI();
        return false;
    }
}

bool ScreenCapture::FallbackToGDI() {
    try {
        // Get primary monitor info (for multi-monitor support, we would need to use EnumDisplayMonitors)
        MONITORINFO monitorInfo = {0};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        
        if (m_monitorIndex == 0) {
            // Primary monitor
            if (!GetMonitorInfo(MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY), &monitorInfo)) {
                std::cerr << "Failed to get monitor info" << std::endl;
                return false;
            }
        } else {
            // Try to find the requested monitor
            struct EnumMonitorsContext {
                int targetIndex;
                int currentIndex;
                MONITORINFO* result;
                bool found;
            };
            
            EnumMonitorsContext context = {0};
            context.targetIndex = m_monitorIndex;
            context.currentIndex = 0;
            context.result = &monitorInfo;
            context.found = false;
            
            EnumDisplayMonitors(
                nullptr, 
                nullptr, 
                [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL {
                    auto context = reinterpret_cast<EnumMonitorsContext*>(lParam);
                    if (context->currentIndex == context->targetIndex) {
                        context->result->cbSize = sizeof(MONITORINFO);
                        context->found = GetMonitorInfo(hMonitor, context->result);
                        return FALSE; // Stop enumeration
                    }
                    context->currentIndex++;
                    return TRUE; // Continue enumeration
                }, 
                reinterpret_cast<LPARAM>(&context)
            );
            
            if (!context.found) {
                std::cerr << "Failed to find monitor with index " << m_monitorIndex << std::endl;
                return false;
            }
        }
        
        // Store monitor dimensions and position
        m_monitorRect = monitorInfo.rcMonitor;
        m_monitorWidth = m_monitorRect.right - m_monitorRect.left;
        m_monitorHeight = m_monitorRect.bottom - m_monitorRect.top;
        
        // Create device contexts
        m_hdcScreen = CreateDC(TEXT("DISPLAY"), nullptr, nullptr, nullptr);
        if (!m_hdcScreen) {
            std::cerr << "Failed to create screen DC" << std::endl;
            return false;
        }
        
        m_hdcMemory = CreateCompatibleDC(m_hdcScreen);
        if (!m_hdcMemory) {
            std::cerr << "Failed to create memory DC" << std::endl;
            return false;
        }
        
        // Create bitmap
        m_hBitmap = CreateCompatibleBitmap(m_hdcScreen, m_monitorWidth, m_monitorHeight);
        if (!m_hBitmap) {
            std::cerr << "Failed to create compatible bitmap" << std::endl;
            return false;
        }
        
        // Select bitmap into memory DC
        SelectObject(m_hdcMemory, m_hBitmap);
        
        std::cout << "GDI initialization successful (" << m_monitorWidth << "x" << m_monitorHeight << ")" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception during GDI initialization: " << e.what() << std::endl;
        CleanupGDI();
        return false;
    }
}

bool ScreenCapture::CaptureFrame(std::vector<uint8_t>& outputBuffer, int& width, int& height) {
    // Calculate framerate
    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);
    
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    
    // Calculate time delta
    double delta = (double)(currentTime.QuadPart - m_lastCaptureTime.QuadPart) / (double)frequency.QuadPart;
    
    // Update capture time if significant time has passed
    if (delta > 0.5) {
        m_framerate = (float)(m_frameCount / delta);
        m_frameCount = 0;
        m_lastCaptureTime = currentTime;
    }
    
    // Increment frame counter
    m_frameCount++;
    
    // Perform the actual capture
    if (m_usingDXGI) {
        if (!CaptureDXGI(outputBuffer, width, height)) {
            std::cout << "DXGI capture failed, falling back to GDI" << std::endl;
            m_usingDXGI = false;
            if (!m_hdcScreen) {
                if (!FallbackToGDI()) {
                    return false;
                }
            }
            return CaptureGDI(outputBuffer, width, height);
        }
        return true;
    } else {
        return CaptureGDI(outputBuffer, width, height);
    }
}

bool ScreenCapture::CaptureDXGI(std::vector<uint8_t>& outputBuffer, int& width, int& height) {
    if (!m_dxgiOutputDuplication) {
        return false;
    }
    
    HRESULT hr = S_OK;
    
    // Release the previous frame
    if (m_acquiredDesktopImage) {
        m_acquiredDesktopImage->Release();
        m_acquiredDesktopImage = nullptr;
    }
    
    // Get the next frame
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    
    // Try to acquire the next frame within a timeout period
    const int MAX_ACQUIRE_ATTEMPTS = 3;
    for (int attempt = 0; attempt < MAX_ACQUIRE_ATTEMPTS; attempt++) {
        hr = m_dxgiOutputDuplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
        
        if (SUCCEEDED(hr)) {
            break;
        }
        
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Timeout is normal when there are no changes
            if (attempt == MAX_ACQUIRE_ATTEMPTS - 1) {
                return false; // No frame changes after multiple attempts
            }
            continue;
        } 
        else if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Access lost, need to recreate duplication
            CleanupDXGI();
            if (!InitializeDXGI()) {
                m_usingDXGI = false;
                return false;
            }
            return CaptureDXGI(outputBuffer, width, height); // Recursive call after reinitialization
        }
        else {
            // Unexpected error
            std::cerr << "Failed to acquire next frame: " << std::hex << hr << std::endl;
            return false;
        }
    }
    
    if (FAILED(hr)) {
        return false;
    }
    
    // Get the desktop image from the resource
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&m_acquiredDesktopImage));
    desktopResource->Release();
    
    if (FAILED(hr)) {
        std::cerr << "Failed to QI for ID3D11Texture2D: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    m_acquiredDesktopImage->GetDesc(&desc);
    
    // Create staging texture for CPU access if not already created or if size changed
    bool needNewStagingTexture = false;
    if (!m_stagingTextures[m_currentTextureIndex]) {
        needNewStagingTexture = true;
    } else {
        D3D11_TEXTURE2D_DESC stagingDesc;
        m_stagingTextures[m_currentTextureIndex]->GetDesc(&stagingDesc);
        
        if (stagingDesc.Width != desc.Width || stagingDesc.Height != desc.Height) {
            m_stagingTextures[m_currentTextureIndex]->Release();
            m_stagingTextures[m_currentTextureIndex] = nullptr;
            needNewStagingTexture = true;
        }
    }
    
    if (needNewStagingTexture) {
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = desc.Width;
        stagingDesc.Height = desc.Height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = desc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;
        
        hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTextures[m_currentTextureIndex]);
        if (FAILED(hr)) {
            std::cerr << "Failed to create staging texture: " << std::hex << hr << std::endl;
            m_dxgiOutputDuplication->ReleaseFrame();
            return false;
        }
    }
    
    // Copy the acquired image to the staging texture
    m_d3dContext->CopyResource(m_stagingTextures[m_currentTextureIndex], m_acquiredDesktopImage);
    
    // Map the staging texture to get access to the data
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = m_d3dContext->Map(m_stagingTextures[m_currentTextureIndex], 0, D3D11_MAP_READ, 0, &mappedResource);
    
    if (FAILED(hr)) {
        std::cerr << "Failed to map staging texture: " << std::hex << hr << std::endl;
        m_dxgiOutputDuplication->ReleaseFrame();
        return false;
    }
    
    // Update output dimensions
    width = desc.Width;
    height = desc.Height;
    
    // Resize output buffer if needed and ensure it's aligned
    size_t requiredSize = width * height * 4; // BGRA format (4 bytes per pixel)
    uint8_t* alignedDst = AlignBuffer(outputBuffer, requiredSize);
    
    // Copy the data
    uint8_t* src = static_cast<uint8_t*>(mappedResource.pData);
    
    if (mappedResource.RowPitch == width * 4) {
        // Rows are packed with no padding, can copy the entire buffer at once
        memcpy(alignedDst, src, requiredSize);
    } else {
        // Rows have padding, need to copy each row separately
        // Use aligned destination pointer for better cache performance
        for (int y = 0; y < height; y++) {
            memcpy(alignedDst + y * width * 4, src + y * mappedResource.RowPitch, width * 4);
        }
    }
    
    // Unmap the texture
    m_d3dContext->Unmap(m_stagingTextures[m_currentTextureIndex], 0);
    
    // Add cursor to the frame if mouse is visible
    if (m_captureCursor) {
        // Get current time for cursor timeout handling
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        
        // Update cursor visibility and position from frame info if available
        if (frameInfo.LastMouseUpdateTime.QuadPart != 0) {
            // Update cursor from DXGI information
            m_cursorVisible = frameInfo.PointerPosition.Visible != 0;
            
            if (m_cursorVisible) {
                // Update cursor position
                m_cursorPosition.x = frameInfo.PointerPosition.Position.x;
                m_cursorPosition.y = frameInfo.PointerPosition.Position.y;
                
                // Update last cursor time
                m_lastCursorUpdateTime = currentTime;
            }
        } else {
            // If DXGI doesn't provide cursor updates, we need to handle it:
            
            // Check if cursor hasn't been updated recently (over 100ms)
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            double timeSinceLastCursorUpdate = 
                (double)(currentTime.QuadPart - m_lastCursorUpdateTime.QuadPart) / 
                (double)frequency.QuadPart;
            
            // If cursor info is stale (over 100ms) or cursor isn't visible, try to get it from system
            if (timeSinceLastCursorUpdate > 0.1 || !m_cursorVisible) {
                POINT position;
                CURSORINFO cursorInfo = {0};
                cursorInfo.cbSize = sizeof(CURSORINFO);
                
                if (GetCursorPos(&position) && ::GetCursorInfo(&cursorInfo) && 
                    (cursorInfo.flags & CURSOR_SHOWING)) {
                    m_cursorPosition = position;
                    m_cursorVisible = true;
                    m_lastCursorUpdateTime = currentTime;
                }
            }
        }
        
        // Always render the cursor if it's visible, even if position hasn't changed
        if (m_cursorVisible) {
            RenderCursorToFrame(outputBuffer, width, height);
        }
    }
    
    // Release the frame
    hr = m_dxgiOutputDuplication->ReleaseFrame();
    if (FAILED(hr)) {
        std::cerr << "Failed to release frame: " << std::hex << hr << std::endl;
        
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Need to recreate the duplication
            CleanupDXGI();
            InitializeDXGI();
        }
        
        // Return true anyway since we've already got the frame data
    }
    
    // Switch to the next texture based on the buffering mode
    switch (m_bufferingMode) {
        case BufferingMode::Single:
            // In single buffering mode, we always use the same texture (index 0)
            m_currentTextureIndex = 0;
            break;
            
        case BufferingMode::Double:
            // In double buffering mode, we alternate between textures 0 and 1
            m_currentTextureIndex = (m_currentTextureIndex + 1) % 2;
            break;
            
        case BufferingMode::Triple:
            // In triple buffering mode, we cycle through all three textures
            m_currentTextureIndex = (m_currentTextureIndex + 1) % 3;
            break;
    }
    
    return true;
}

bool ScreenCapture::CaptureGDI(std::vector<uint8_t>& outputBuffer, int& width, int& height) {
    if (!m_hdcScreen || !m_hdcMemory || !m_hBitmap) {
        return false;
    }
    
    try {
        // Copy screen to bitmap
        if (!BitBlt(m_hdcMemory, 0, 0, m_monitorWidth, m_monitorHeight, 
                   m_hdcScreen, m_monitorRect.left, m_monitorRect.top, SRCCOPY)) {
            std::cerr << "BitBlt failed with error: " << GetLastError() << std::endl;
            return false;
        }
        
        // Get bitmap info
        BITMAP bmpInfo;
        if (!GetObject(m_hBitmap, sizeof(BITMAP), &bmpInfo)) {
            std::cerr << "GetObject failed with error: " << GetLastError() << std::endl;
            return false;
        }
        
        // Prepare BITMAPINFO structure
        BITMAPINFOHEADER bi;
        ZeroMemory(&bi, sizeof(BITMAPINFOHEADER));
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = bmpInfo.bmWidth;
        bi.biHeight = -bmpInfo.bmHeight; // Negative for top-down
        bi.biPlanes = 1;
        bi.biBitCount = 32; // BGRA
        bi.biCompression = BI_RGB;
        
        // Resize output buffer
        width = bmpInfo.bmWidth;
        height = bmpInfo.bmHeight;
        const size_t bytesPerPixel = 4; // BGRA format
        const size_t bufferSize = width * height * bytesPerPixel;
        outputBuffer.resize(bufferSize);
        
        // Get bitmap bits
        if (!GetDIBits(m_hdcMemory, m_hBitmap, 0, height, outputBuffer.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
            std::cerr << "GetDIBits failed with error: " << GetLastError() << std::endl;
            return false;
        }
        
        // Render mouse cursor if enabled
        if (m_captureCursor) {
            // For GDI capture, always update cursor position
            POINT position;
            CURSORINFO cursorInfo = {0};
            cursorInfo.cbSize = sizeof(CURSORINFO);
            
            if (GetCursorPos(&position) && ::GetCursorInfo(&cursorInfo) && 
                (cursorInfo.flags & CURSOR_SHOWING)) {
                m_cursorPosition = position;
                m_cursorVisible = true;
                
                // Update cursor timestamp
                QueryPerformanceCounter(&m_lastCursorUpdateTime);
            } else {
                m_cursorVisible = false;
            }
            
            if (m_cursorVisible) {
                RenderCursorToFrame(outputBuffer, width, height);
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception during GDI capture: " << e.what() << std::endl;
        return false;
    }
}

void ScreenCapture::CleanupDXGI() {
    if (m_dxgiOutputDuplication) {
        m_dxgiOutputDuplication->Release();
        m_dxgiOutputDuplication = nullptr;
    }
    
    if (m_stagingTextures[0]) {
        m_stagingTextures[0]->Release();
        m_stagingTextures[0] = nullptr;
    }
    
    if (m_stagingTextures[1]) {
        m_stagingTextures[1]->Release();
        m_stagingTextures[1] = nullptr;
    }
    
    if (m_stagingTextures[2]) {
        m_stagingTextures[2]->Release();
        m_stagingTextures[2] = nullptr;
    }
    
    if (m_acquiredDesktopImage) {
        m_acquiredDesktopImage->Release();
        m_acquiredDesktopImage = nullptr;
    }
    
    if (m_d3dContext) {
        m_d3dContext->Release();
        m_d3dContext = nullptr;
    }
    
    if (m_d3dDevice) {
        m_d3dDevice->Release();
        m_d3dDevice = nullptr;
    }
}

void ScreenCapture::CleanupGDI() {
    if (m_hBitmap) {
        DeleteObject(m_hBitmap);
        m_hBitmap = nullptr;
    }
    
    if (m_hdcMemory) {
        DeleteDC(m_hdcMemory);
        m_hdcMemory = nullptr;
    }
    
    if (m_hdcScreen) {
        DeleteDC(m_hdcScreen);
        m_hdcScreen = nullptr;
    }
}

// Add a method to get the current framerate
float ScreenCapture::GetFrameRate() const {
    return m_framerate;
}

void ScreenCapture::RenderCursorToFrame(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight) {
    if (!m_cursorVisible) {
        return;  // Don't render if cursor is not visible
    }
    
    // Calculate the cursor position relative to the captured area
    POINT adjustedPosition = m_cursorPosition;
    adjustedPosition.x -= m_monitorRect.left;
    adjustedPosition.y -= m_monitorRect.top;
    
    // Get system cursor
    POINT position;
    HCURSOR cursor;
    if (GetCursorInfo(position, cursor)) {
        // Adjust position for monitor offset
        position.x -= m_monitorRect.left;
        position.y -= m_monitorRect.top;
        DrawCursor(frameData, frameWidth, frameHeight, position, cursor);
    }
}

bool ScreenCapture::GetCursorInfo(POINT& position, HCURSOR& cursor) {
    // Get cursor position
    if (!GetCursorPos(&position)) {
        std::cerr << "Failed to get cursor position: " << GetLastError() << std::endl;
        return false;
    }
    
    // Get cursor info
    CURSORINFO cursorInfo = {0};
    cursorInfo.cbSize = sizeof(CURSORINFO);
    
    if (!::GetCursorInfo(&cursorInfo)) {
        std::cerr << "Failed to get cursor info: " << GetLastError() << std::endl;
        return false;
    }
    
    // Check if cursor is visible
    if (!(cursorInfo.flags & CURSOR_SHOWING)) {
        return false;
    }
    
    // Get cursor handle
    cursor = cursorInfo.hCursor;
    return true;
}

void ScreenCapture::DrawCursor(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                              const POINT& position, HCURSOR cursor) {
    // Debug the cursor type - We'll remove this as it's causing linter errors
    // wchar_t cursorName[256] = {0};
    // DWORD actualSize = GetCursorInfo ? GetCursorInfo(cursor, cursorName, 256) : 0;
    
    ICONINFO iconInfo = {0};
    if (!GetIconInfo(cursor, &iconInfo)) {
        std::cerr << "Failed to get icon info: " << GetLastError() << std::endl;
        return;
    }
    
    // Special handling for the text edit cursor (I-beam)
    HCURSOR ibeamCursor = LoadCursor(nullptr, IDC_IBEAM);
    if (cursor == ibeamCursor) {
        DrawIBeamCursor(frameData, frameWidth, frameHeight, position);
        DestroyCursor(ibeamCursor);
        return;
    }
    DestroyCursor(ibeamCursor);
    
    // Get cursor dimensions and hotspot
    BITMAP bmpInfo;
    if (!GetObject(iconInfo.hbmMask, sizeof(BITMAP), &bmpInfo)) {
        std::cerr << "Failed to get cursor bitmap info: " << GetLastError() << std::endl;
        if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        return;
    }
    
    const int cursorWidth = bmpInfo.bmWidth;
    const int cursorHeight = iconInfo.hbmColor ? bmpInfo.bmHeight : bmpInfo.bmHeight / 2;
    
    // Adjust position by hotspot
    const int hotspotX = iconInfo.xHotspot;
    const int hotspotY = iconInfo.yHotspot;
    
    // Get cursor image data
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    
    // Select the color bitmap into the memory DC
    HBITMAP hbmpOld = nullptr;
    bool hasAlpha = false;
    
    if (iconInfo.hbmColor) {
        // Color cursor
        hbmpOld = (HBITMAP)SelectObject(hdcMem, iconInfo.hbmColor);
        hasAlpha = true;
    } else {
        // Monochrome cursor
        hbmpOld = (HBITMAP)SelectObject(hdcMem, iconInfo.hbmMask);
        hasAlpha = false;
    }
    
    // Create a buffer for the cursor image
    std::vector<uint32_t> cursorData(cursorWidth * cursorHeight, 0);
    
    // Get cursor bitmap bits
    BITMAPINFOHEADER bmi = {0};
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = cursorWidth;
    bmi.biHeight = -cursorHeight; // Negative for top-down
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;
    bmi.biCompression = BI_RGB;
    
    // For the I-beam cursor or other special cursors, we may need to handle them differently
    bool success = GetDIBits(hdcMem, iconInfo.hbmColor ? iconInfo.hbmColor : iconInfo.hbmMask,
              0, cursorHeight, cursorData.data(), (BITMAPINFO*)&bmi, DIB_RGB_COLORS);
    
    if (!success) {
        std::cerr << "Failed to get DIBits for cursor: " << GetLastError() << std::endl;
        
        // Try with the default system cursor for I-beam as a fallback
        HCURSOR fallbackCursor = LoadCursor(nullptr, IDC_IBEAM);
        if (cursor == fallbackCursor) {
            // Draw a simple I-beam cursor manually
            DrawIBeamCursor(frameData, frameWidth, frameHeight, position);
        }
        DestroyCursor(fallbackCursor);
        
        // Clean up resources
        SelectObject(hdcMem, hbmpOld);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        
        if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        return;
    }
    
    // Clean up cursor resources
    SelectObject(hdcMem, hbmpOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    
    // Calculate cursor position in frame
    const int startX = position.x - hotspotX;
    const int startY = position.y - hotspotY;
    
    // Draw cursor onto frame
    const int bytesPerPixel = 4;
    
    for (int y = 0; y < cursorHeight; ++y) {
        const int frameY = startY + y;
        if (frameY < 0 || frameY >= frameHeight) continue;
        
        for (int x = 0; x < cursorWidth; ++x) {
            const int frameX = startX + x;
            if (frameX < 0 || frameX >= frameWidth) continue;
            
            const int cursorIndex = y * cursorWidth + x;
            const int frameIndex = (frameY * frameWidth + frameX) * bytesPerPixel;
            
            const uint32_t cursorPixel = cursorData[cursorIndex];
            
            // For monochrome cursors, we need to handle them differently
            if (!hasAlpha) {
                // For AND mask (black where cursor will be rendered)
                const bool maskBit = (cursorPixel & 0xFF) == 0;
                if (!maskBit) continue; // Skip transparent parts
                
                // Invert the pixel color for XOR mask
                frameData[frameIndex] = 255 - frameData[frameIndex];     // B
                frameData[frameIndex + 1] = 255 - frameData[frameIndex + 1]; // G
                frameData[frameIndex + 2] = 255 - frameData[frameIndex + 2]; // R
                // Keep alpha unchanged
            } else {
                // For color cursors with alpha
                const uint8_t alpha = (cursorPixel >> 24) & 0xFF;
                if (alpha == 0) continue; // Skip fully transparent pixels
                
                if (alpha == 255) {
                    // Fully opaque, just copy
                    frameData[frameIndex] = cursorPixel & 0xFF;         // B
                    frameData[frameIndex + 1] = (cursorPixel >> 8) & 0xFF;  // G
                    frameData[frameIndex + 2] = (cursorPixel >> 16) & 0xFF; // R
                    frameData[frameIndex + 3] = 255;                        // A
                } else {
                    // Alpha blending
                    const float alphaF = alpha / 255.0f;
                    const uint8_t srcB = cursorPixel & 0xFF;
                    const uint8_t srcG = (cursorPixel >> 8) & 0xFF;
                    const uint8_t srcR = (cursorPixel >> 16) & 0xFF;
                    
                    const uint8_t dstB = frameData[frameIndex];
                    const uint8_t dstG = frameData[frameIndex + 1];
                    const uint8_t dstR = frameData[frameIndex + 2];
                    
                    frameData[frameIndex] = static_cast<uint8_t>(srcB * alphaF + dstB * (1.0f - alphaF));
                    frameData[frameIndex + 1] = static_cast<uint8_t>(srcG * alphaF + dstG * (1.0f - alphaF));
                    frameData[frameIndex + 2] = static_cast<uint8_t>(srcR * alphaF + dstR * (1.0f - alphaF));
                    // Keep destination alpha
                }
            }
        }
    }
}

void ScreenCapture::DrawIBeamCursor(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, const POINT& position) {
    const int cursorHeight = 21;
    const int cursorWidth = 11;
    const int hotspotX = 5;
    const int hotspotY = 10;
    
    const int startX = position.x - hotspotX;
    const int startY = position.y - hotspotY;
    
    const int bytesPerPixel = 4;
    
    // Draw a simple I-beam cursor (white with black outline)
    // Draw horizontal lines at top and bottom
    for (int x = 0; x < cursorWidth; x++) {
        // Top line
        DrawCursorPixel(frameData, frameWidth, frameHeight, startX + x, startY, bytesPerPixel, 0, 0, 0);
        DrawCursorPixel(frameData, frameWidth, frameHeight, startX + x, startY + 1, bytesPerPixel, 255, 255, 255);
        
        // Bottom line
        DrawCursorPixel(frameData, frameWidth, frameHeight, startX + x, startY + cursorHeight - 2, bytesPerPixel, 255, 255, 255);
        DrawCursorPixel(frameData, frameWidth, frameHeight, startX + x, startY + cursorHeight - 1, bytesPerPixel, 0, 0, 0);
    }
    
    // Draw vertical line in the middle
    for (int y = 2; y < cursorHeight - 2; y++) {
        // Left outline
        DrawCursorPixel(frameData, frameWidth, frameHeight, startX + 4, startY + y, bytesPerPixel, 0, 0, 0);
        // Center
        DrawCursorPixel(frameData, frameWidth, frameHeight, startX + 5, startY + y, bytesPerPixel, 255, 255, 255);
        // Right outline
        DrawCursorPixel(frameData, frameWidth, frameHeight, startX + 6, startY + y, bytesPerPixel, 0, 0, 0);
    }
}

void ScreenCapture::DrawCursorPixel(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                                   int x, int y, int bytesPerPixel, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= frameWidth || y < 0 || y >= frameHeight) {
        return;
    }
    
    const int frameIndex = (y * frameWidth + x) * bytesPerPixel;
    
    frameData[frameIndex] = b;     // B
    frameData[frameIndex + 1] = g; // G
    frameData[frameIndex + 2] = r; // R
    frameData[frameIndex + 3] = 255; // A
}

void ScreenCapture::SetBufferingMode(BufferingMode mode) {
    // Only change buffering mode if it's different from the current one
    if (mode != m_bufferingMode) {
        m_bufferingMode = mode;
        
        // Log the change
        std::cout << "Switching to ";
        switch (m_bufferingMode) {
            case BufferingMode::Single:
                std::cout << "single";
                break;
            case BufferingMode::Double:
                std::cout << "double";
                break;
            case BufferingMode::Triple:
                std::cout << "triple";
                break;
        }
        std::cout << " buffering mode" << std::endl;
    }
}

// Calculate the required size for a buffer, including extra padding for alignment
size_t ScreenCapture::GetAlignedSize(size_t size) const {
    return size + m_alignedBufferPadding;
}

// Get aligned pointer from buffer
uint8_t* ScreenCapture::AlignBuffer(std::vector<uint8_t>& buffer, size_t requiredSize) {
    // Resize the buffer to include extra padding for alignment
    size_t alignedSize = GetAlignedSize(requiredSize);
    
    if (buffer.size() < alignedSize) {
        buffer.resize(alignedSize);
    }
    
    // Calculate the aligned pointer
    uintptr_t address = reinterpret_cast<uintptr_t>(buffer.data());
    uintptr_t alignedAddress = (address + MEMORY_ALIGNMENT - 1) & ~(MEMORY_ALIGNMENT - 1);
    
    // Make sure we have enough space in the buffer
    if (alignedAddress + requiredSize > address + buffer.size()) {
        // If we don't have enough space, resize the buffer again
        size_t extraPadding = alignedAddress - address;
        buffer.resize(requiredSize + extraPadding);
        alignedAddress = (reinterpret_cast<uintptr_t>(buffer.data()) + MEMORY_ALIGNMENT - 1) & ~(MEMORY_ALIGNMENT - 1);
    }
    
    return reinterpret_cast<uint8_t*>(alignedAddress);
} 