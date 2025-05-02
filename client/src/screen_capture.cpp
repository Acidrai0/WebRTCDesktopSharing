#include "screen_capture.h"

#include <iostream>
#include <memory>
#include <stdexcept>

ScreenCapture::ScreenCapture() {
    m_usingDXGI = true;
    m_captureCursor = true;
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
    
    try {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        IDXGIResource* desktopResource = nullptr;
        
        // Timeout in milliseconds
        UINT timeoutMs = 100;
        
        // Acquire next frame
        HRESULT hr = m_dxgiOutputDuplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);
        
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // No new frame available, not an error
            return true;
        } else if (FAILED(hr)) {
            std::cerr << "Failed to acquire next frame: " << std::hex << hr << std::endl;
            return false;
        }
        
        // Get texture
        ID3D11Texture2D* desktopTexture = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktopTexture));
        desktopResource->Release();
        
        if (FAILED(hr)) {
            std::cerr << "Failed to query interface for ID3D11Texture2D: " << std::hex << hr << std::endl;
            m_dxgiOutputDuplication->ReleaseFrame();
            return false;
        }
        
        // Get texture description
        D3D11_TEXTURE2D_DESC textureDesc;
        desktopTexture->GetDesc(&textureDesc);
        
        // Create staging texture for CPU access
        ID3D11Texture2D* stagingTexture = nullptr;
        textureDesc.Usage = D3D11_USAGE_STAGING;
        textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        textureDesc.BindFlags = 0;
        textureDesc.MiscFlags = 0;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.SampleDesc.Count = 1;
        
        hr = m_d3dDevice->CreateTexture2D(&textureDesc, nullptr, &stagingTexture);
        
        if (FAILED(hr)) {
            std::cerr << "Failed to create staging texture: " << std::hex << hr << std::endl;
            desktopTexture->Release();
            m_dxgiOutputDuplication->ReleaseFrame();
            return false;
        }
        
        // Copy desktop texture to staging texture
        m_d3dContext->CopyResource(stagingTexture, desktopTexture);
        desktopTexture->Release();
        
        // Map staging texture
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        hr = m_d3dContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
        
        if (FAILED(hr)) {
            std::cerr << "Failed to map staging texture: " << std::hex << hr << std::endl;
            stagingTexture->Release();
            m_dxgiOutputDuplication->ReleaseFrame();
            return false;
        }
        
        // Copy data to output buffer
        width = textureDesc.Width;
        height = textureDesc.Height;
        
        const size_t bytesPerPixel = 4; // BGRA format
        const size_t bufferSize = width * height * bytesPerPixel;
        outputBuffer.resize(bufferSize);
        
        const uint8_t* src = static_cast<const uint8_t*>(mappedResource.pData);
        uint8_t* dst = outputBuffer.data();
        
        for (int y = 0; y < height; ++y) {
            memcpy(dst, src, width * bytesPerPixel);
            src += mappedResource.RowPitch;
            dst += width * bytesPerPixel;
        }
        
        // Unmap and release resources
        m_d3dContext->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        m_dxgiOutputDuplication->ReleaseFrame();
        
        // Render mouse cursor if enabled
        if (m_captureCursor) {
            RenderCursorToFrame(outputBuffer, width, height);
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception during DXGI capture: " << e.what() << std::endl;
        return false;
    }
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
            RenderCursorToFrame(outputBuffer, width, height);
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception during GDI capture: " << e.what() << std::endl;
        return false;
    }
}

void ScreenCapture::RenderCursorToFrame(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight) {
    // Get cursor info
    POINT cursorPos;
    HCURSOR cursor;
    
    if (GetCursorInfo(cursorPos, cursor)) {
        // Convert cursor position to monitor relative
        cursorPos.x -= m_monitorRect.left;
        cursorPos.y -= m_monitorRect.top;
        
        // Draw cursor
        DrawCursor(frameData, frameWidth, frameHeight, cursorPos, cursor);
    }
}

bool ScreenCapture::GetCursorInfo(POINT& position, HCURSOR& cursor) {
    // Get cursor position
    if (!GetCursorPos(&position)) {
        std::cerr << "Failed to get cursor position: " << GetLastError() << std::endl;
        return false;
    }
    
    // Get cursor handle
    CURSORINFO cursorInfo = {0};
    cursorInfo.cbSize = sizeof(CURSORINFO);
    
    if (!::GetCursorInfo(&cursorInfo)) {
        std::cerr << "Failed to get cursor info: " << GetLastError() << std::endl;
        return false;
    }
    
    if (!(cursorInfo.flags & CURSOR_SHOWING)) {
        // Cursor is hidden
        return false;
    }
    
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
        return;
    }
    
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
        if (cursor == ibeamCursor) {
            // Draw a simple I-beam cursor manually
            DrawIBeamCursor(frameData, frameWidth, frameHeight, position);
        }
        
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

// Helper method to draw an I-beam cursor manually
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

// Helper to draw a single pixel for the cursor with bounds checking
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

void ScreenCapture::CleanupDXGI() {
    if (m_dxgiOutputDuplication) {
        m_dxgiOutputDuplication->Release();
        m_dxgiOutputDuplication = nullptr;
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