#include "screen_capture.h"
#include "simd_mem_utils.h" // Include our SIMD utilities
#include "xsimd_mem_utils.h" // Include our xsimd utilities

#include <iostream>
#include <memory>
#include <stdexcept>
#include <algorithm> // For std::min_element and std::max_element

ScreenCapture::ScreenCapture() {
    m_usingDXGI = true;
    m_captureCursor = true;
    m_cursorVisible = false;
    m_acquiredDesktopImage = nullptr;
    m_stagingTextures[0] = nullptr;
    m_stagingTextures[1] = nullptr;
    m_currentTextureIndex = 0;
    m_frameCount = 0;
    m_framerate = 0.0f;
    m_bufferingMode = BufferingMode::Double; // Using double buffering
    m_alignedBufferPadding = MEMORY_ALIGNMENT; // Set default padding for alignment
    
    // Initialize buffer pool
    m_lastWidth = 0;
    m_lastHeight = 0;
    m_unusedTime = 0;
    m_lastUnusedCheck.QuadPart = 0;
    
    // Initialize cursor position
    GetCursorPos(&m_cursorPosition);
    
    // Initialize performance counter for framerate calculation
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&m_lastCaptureTime);
    
    // Initialize cursor update time
    m_lastCursorUpdateTime = m_lastCaptureTime;
    
    // Initialize SIMD utilities - this is now the default implementation
    SimdMemUtils::Initialize();
    
    // Also initialize xsimd utilities in case we need them later
    XSimdMemUtils::Initialize();
    
    std::cout << "Screen capture initialized using SIMD optimizations by default" << std::endl;
}

ScreenCapture::~ScreenCapture() {
    CleanupDXGI();
    CleanupGDI();
    
    // Clean up buffer pool
    for (auto& buffer : m_bufferPool) {
        buffer.clear();
    }
    m_bufferPool.clear();
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
        std::cout << "Initializing GDI screen capture for monitor " << m_monitorIndex << std::endl;
        
        // Count monitors for better error messages
        int monitorCount = GetSystemMetrics(SM_CMONITORS);
        std::cout << "System has " << monitorCount << " monitor(s)" << std::endl;
        
        if (m_monitorIndex >= monitorCount) {
            std::cerr << "Monitor index " << m_monitorIndex << " is out of range, defaulting to primary monitor" << std::endl;
            m_monitorIndex = 0;
        }
        
        // Get monitor info
        MONITORINFO monitorInfo = {0};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        HMONITOR targetMonitor = nullptr;
        
        if (m_monitorIndex == 0) {
            // Primary monitor
            targetMonitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
            if (!targetMonitor) {
                std::cerr << "Failed to get primary monitor handle, error: " << GetLastError() << std::endl;
                return false;
            }
            
            if (!GetMonitorInfo(targetMonitor, &monitorInfo)) {
                std::cerr << "Failed to get primary monitor info, error: " << GetLastError() << std::endl;
                return false;
            }
            
            std::cout << "Using primary monitor" << std::endl;
        } else {
            // Enumerate all monitors to find the requested one
            // Define a struct to carry information through the EnumDisplayMonitors callback
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
                std::cerr << "Failed to find monitor with index " << m_monitorIndex << ", defaulting to primary" << std::endl;
                targetMonitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
                if (!GetMonitorInfo(targetMonitor, &monitorInfo)) {
                    std::cerr << "Failed to get fallback monitor info, error: " << GetLastError() << std::endl;
                    return false;
                }
            } else {
                std::cout << "Found monitor with index " << m_monitorIndex << std::endl;
            }
        }
        
        // Store monitor dimensions and position
        m_monitorRect = monitorInfo.rcMonitor;
        m_monitorWidth = m_monitorRect.right - m_monitorRect.left;
        m_monitorHeight = m_monitorRect.bottom - m_monitorRect.top;
        
        std::cout << "Monitor dimensions: " << m_monitorWidth << "x" << m_monitorHeight << std::endl;
        std::cout << "Monitor position: Left=" << m_monitorRect.left << ", Top=" << m_monitorRect.top 
                  << ", Right=" << m_monitorRect.right << ", Bottom=" << m_monitorRect.bottom << std::endl;
        
        // Create device contexts - using GetDC(NULL) for the entire virtual screen
        // which is more reliable than CreateDC("DISPLAY"...)
        m_hdcScreen = GetDC(NULL);
        if (!m_hdcScreen) {
            std::cerr << "Failed to create screen DC, error: " << GetLastError() << std::endl;
            return false;
        }
        
        m_hdcMemory = CreateCompatibleDC(m_hdcScreen);
        if (!m_hdcMemory) {
            std::cerr << "Failed to create memory DC, error: " << GetLastError() << std::endl;
            ReleaseDC(NULL, m_hdcScreen);
            m_hdcScreen = nullptr;
            return false;
        }
        
        // Create bitmap
        m_hBitmap = CreateCompatibleBitmap(m_hdcScreen, m_monitorWidth, m_monitorHeight);
        if (!m_hBitmap) {
            std::cerr << "Failed to create compatible bitmap, error: " << GetLastError() << std::endl;
            DeleteDC(m_hdcMemory);
            ReleaseDC(NULL, m_hdcScreen);
            m_hdcMemory = nullptr;
            m_hdcScreen = nullptr;
            return false;
        }
        
        // Select bitmap into memory DC
        HBITMAP oldBmp = (HBITMAP)SelectObject(m_hdcMemory, m_hBitmap);
        if (!oldBmp) {
            std::cerr << "Failed to select bitmap into DC, error: " << GetLastError() << std::endl;
            DeleteObject(m_hBitmap);
            DeleteDC(m_hdcMemory);
            ReleaseDC(NULL, m_hdcScreen);
            m_hBitmap = nullptr;
            m_hdcMemory = nullptr;
            m_hdcScreen = nullptr;
            return false;
        }
        
        std::cout << "GDI initialization successful for monitor " << m_monitorIndex 
                 << " (" << m_monitorWidth << "x" << m_monitorHeight << ")" << std::endl;
        
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
    
    // Log the format for debugging purposes
    const char* formatName = "Unknown";
    switch (desc.Format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: formatName = "DXGI_FORMAT_B8G8R8A8_UNORM (BGRA)"; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: formatName = "DXGI_FORMAT_R8G8B8A8_UNORM (RGBA)"; break;
        default: formatName = "Other format"; break;
    }
    static bool formatLogged = false;
    if (!formatLogged) {
        std::cout << "DXGI Desktop Duplication format: " << formatName << std::endl;
        formatLogged = true;
    }
    
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
        // Always use BGRA format for consistent output regardless of input format
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        // Direct copy if formats match
        m_d3dContext->CopyResource(m_stagingTextures[m_currentTextureIndex], m_acquiredDesktopImage);
    } else {
        // Format conversion needed (e.g., RGBA to BGRA)
        static ID3D11Texture2D* converterTexture = nullptr;
        static DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN;
        static int lastWidth = 0, lastHeight = 0;
        
        // Create or recreate converter texture if needed
        if (!converterTexture || lastFormat != desc.Format || 
            lastWidth != desc.Width || lastHeight != desc.Height) {
            
            // Release any existing converter texture
            if (converterTexture) {
                converterTexture->Release();
                converterTexture = nullptr;
            }
            
            // Create a new texture with BGRA format but renderable
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = desc.Width;
            texDesc.Height = desc.Height;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.SampleDesc.Quality = 0;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            texDesc.CPUAccessFlags = 0;
            texDesc.MiscFlags = 0;
            
            hr = m_d3dDevice->CreateTexture2D(&texDesc, nullptr, &converterTexture);
            if (FAILED(hr)) {
                std::cerr << "Failed to create converter texture: " << std::hex << hr << std::endl;
                // Fall back to direct copy and hope for the best
                m_d3dContext->CopyResource(m_stagingTextures[m_currentTextureIndex], m_acquiredDesktopImage);
            } else {
                // Store state for next time
                lastFormat = desc.Format;
                lastWidth = desc.Width;
                lastHeight = desc.Height;
                
                std::cout << "Created format converter texture from " << formatName 
                          << " to DXGI_FORMAT_B8G8R8A8_UNORM" << std::endl;
            }
        }
        
        if (converterTexture) {
            // Perform GPU color conversion
            m_d3dContext->CopyResource(converterTexture, m_acquiredDesktopImage);
            m_d3dContext->CopyResource(m_stagingTextures[m_currentTextureIndex], converterTexture);
        }
    }
    
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
    
    // Make sure output buffer has appropriate size before aligning
    if (outputBuffer.size() < requiredSize) {
        std::cout << "Resizing output buffer from " << outputBuffer.size() << " to " << requiredSize << " bytes" << std::endl;
        outputBuffer.resize(requiredSize);
    }
    
    uint8_t* alignedDst = AlignBuffer(outputBuffer, requiredSize);
    
    // Copy the data using optimized method
    uint8_t* src = static_cast<uint8_t*>(mappedResource.pData);
    OptimizedCopyFrame(alignedDst, src, width, height, mappedResource.RowPitch);
    
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
    
    // Update buffer pool management
    ManageBufferPool(outputBuffer, width, height);
    
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
    
    // Switch to the next texture based on double buffering
    m_currentTextureIndex = (m_currentTextureIndex + 1) % 2;
    
    return true;
}

bool ScreenCapture::CaptureGDI(std::vector<uint8_t>& outputBuffer, int& width, int& height) {
    if (!m_hdcScreen || !m_hdcMemory || !m_hBitmap) {
        std::cerr << "Invalid GDI handles for capture" << std::endl;
        // Try to reinitialize the GDI handles
        CleanupGDI();
        if (!FallbackToGDI()) {
            return false;
        }
    }
    
    try {
        // Copy screen to bitmap
        if (!BitBlt(m_hdcMemory, 0, 0, m_monitorWidth, m_monitorHeight, 
                   m_hdcScreen, m_monitorRect.left, m_monitorRect.top, SRCCOPY)) {
            DWORD error = GetLastError();
            std::cerr << "BitBlt failed with error: " << error << std::endl;
            
            // Provide more detailed information about the error
            switch (error) {
                case ERROR_INVALID_HANDLE:
                    std::cerr << "Invalid handle - Attempting to reinitialize DC handles" << std::endl;
                    CleanupGDI();
                    if (!FallbackToGDI()) {
                        return false;
                    }
                    // Try again with new handles
                    return CaptureGDI(outputBuffer, width, height);
                case ERROR_INVALID_PARAMETER:
                    std::cerr << "Invalid parameter - Check monitor dimensions and positions" << std::endl;
                    // Log the current dimensions for debugging
                    std::cerr << "Monitor rect: left=" << m_monitorRect.left << ", top=" << m_monitorRect.top 
                              << ", right=" << m_monitorRect.right << ", bottom=" << m_monitorRect.bottom << std::endl;
                    std::cerr << "Monitor dimensions: " << m_monitorWidth << "x" << m_monitorHeight << std::endl;
                    break;
                case ERROR_ACCESS_DENIED:
                    std::cerr << "Access denied - The application may not have permission to capture the screen" << std::endl;
                    break;
                default:
                    std::cerr << "Unknown BitBlt error code: " << error << std::endl;
            }
            
            // If we're here, try to reinitialize as a last resort
            CleanupGDI();
            if (!FallbackToGDI()) {
                return false;
            }
            return CaptureGDI(outputBuffer, width, height);
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
        
        // Debug output for buffer resizing
        if (outputBuffer.size() != bufferSize) {
            std::cout << "CaptureGDI: Resizing buffer from " << outputBuffer.size() 
                      << " to " << bufferSize << " bytes" << std::endl;
        }
        
        outputBuffer.resize(bufferSize);
        
        // Get bitmap bits
        if (!GetDIBits(m_hdcMemory, m_hBitmap, 0, height, outputBuffer.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
            DWORD error = GetLastError();
            std::cerr << "GetDIBits failed with error: " << error << std::endl;
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
        
        // Update buffer pool management
        ManageBufferPool(outputBuffer, width, height);
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception during GDI capture: " << e.what() << std::endl;
        return false;
    }
}

void ScreenCapture::CleanupDXGI() {
    // Clean up the static converter texture if it exists
    static ID3D11Texture2D* converterTexture = nullptr;
    if (converterTexture) {
        converterTexture->Release();
        converterTexture = nullptr;
    }
    
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
        // Use ReleaseDC for device contexts obtained with GetDC
        ReleaseDC(NULL, m_hdcScreen);
        m_hdcScreen = nullptr;
    }
    
    std::cout << "GDI resources cleaned up" << std::endl;
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
    // No need to change buffering mode, we only support double buffering
    if (mode != m_bufferingMode) {
        m_bufferingMode = mode;
        std::cout << "Using double buffering mode" << std::endl;
    }
}

// Calculate the required size for a buffer, with improved growth factor
size_t ScreenCapture::GetAlignedSize(size_t size) const {
    // Add padding for alignment plus 50% growth factor for future use
    return size + m_alignedBufferPadding + (size / 2);
}

// Improved AlignBuffer with buffer pooling
uint8_t* ScreenCapture::AlignBuffer(std::vector<uint8_t>& outputBuffer, size_t requiredSize) {
    // Safety check for empty buffer
    if (outputBuffer.empty()) {
        std::cout << "AlignBuffer: Empty output buffer, resizing to " << requiredSize << " bytes" << std::endl;
        outputBuffer.resize(requiredSize);
    }
    
    // Calculate needed aligned size
    size_t alignedSize = requiredSize + m_alignedBufferPadding;
    
    // Check if we can use an existing buffer from the pool
    if (outputBuffer.size() < alignedSize) {
        bool foundSuitableBuffer = false;
        
        // Try to find a suitable buffer in the pool
        for (auto it = m_bufferPool.begin(); it != m_bufferPool.end(); ++it) {
            if (it->size() >= alignedSize) {
                // Found suitable buffer, swap with output buffer
                std::cout << "AlignBuffer: Found suitable buffer in pool, size: " << it->size() << " bytes" << std::endl;
                std::swap(outputBuffer, *it);
                m_bufferPool.erase(it);
                foundSuitableBuffer = true;
                break;
            }
        }
        
        // If no suitable buffer found, resize with growth factor
        if (!foundSuitableBuffer) {
            // Use a 1.5x growth factor to reduce reallocations
            size_t newSize = std::max(alignedSize, outputBuffer.size() + (outputBuffer.size() / 2));
            std::cout << "AlignBuffer: Resizing buffer from " << outputBuffer.size() 
                      << " to " << newSize << " bytes" << std::endl;
            outputBuffer.resize(newSize);
        }
    }
    
    // Calculate aligned pointer in a single pass
    uint8_t* bufferData = outputBuffer.data();
    uintptr_t address = reinterpret_cast<uintptr_t>(bufferData);
    uintptr_t alignedAddress = (address + MEMORY_ALIGNMENT - 1) & ~(static_cast<uintptr_t>(MEMORY_ALIGNMENT - 1));
    
    // Ensure we have enough space after alignment
    if (alignedAddress + requiredSize > address + outputBuffer.size()) {
        // Add exact padding needed and grow with factor
        size_t extraPadding = alignedAddress - address;
        size_t totalNeeded = requiredSize + extraPadding;
        size_t growthSize = totalNeeded + (totalNeeded / 2); // Add 50% extra
        
        std::cout << "AlignBuffer: Need more space after alignment, resizing to " 
                  << growthSize << " bytes" << std::endl;
        outputBuffer.resize(growthSize);
        
        // Recalculate alignment after resize
        bufferData = outputBuffer.data();
        address = reinterpret_cast<uintptr_t>(bufferData);
        alignedAddress = (address + MEMORY_ALIGNMENT - 1) & ~(static_cast<uintptr_t>(MEMORY_ALIGNMENT - 1));
    }
    
    return reinterpret_cast<uint8_t*>(alignedAddress);
}

void ScreenCapture::ManageBufferPool(std::vector<uint8_t>& usedBuffer, int width, int height) {
    const size_t MAX_POOL_SIZE = 3; // Max number of buffers to keep
    const double UNUSED_TIMEOUT_SECONDS = 5.0; // Seconds to keep unused buffers
    
    // Track current frame dimensions
    if (width != m_lastWidth || height != m_lastHeight) {
        // Dimensions changed, update tracking
        m_lastWidth = width;
        m_lastHeight = height;
        
        // Reset unused time counter
        m_unusedTime = 0;
    }
    
    // Add a copy of the used buffer to the pool - DO NOT clear the original
    if (!usedBuffer.empty()) {
        // Only keep a fixed number of buffers in the pool
        if (m_bufferPool.size() < MAX_POOL_SIZE) {
            // Add a copy of the buffer to the pool instead of swapping
            m_bufferPool.push_back(std::vector<uint8_t>(usedBuffer));
        } else {
            // Find smallest buffer to replace
            auto smallestIt = std::min_element(m_bufferPool.begin(), m_bufferPool.end(),
                [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                    return a.size() < b.size();
                });
            
            if (smallestIt != m_bufferPool.end() && smallestIt->size() < usedBuffer.size()) {
                // Replace smaller buffer with a copy of the current one
                *smallestIt = usedBuffer;
            }
        }
    }
    
    // Update unused time tracking and clean up old buffers
    LARGE_INTEGER currentTime, frequency;
    QueryPerformanceCounter(&currentTime);
    QueryPerformanceFrequency(&frequency);
    
    if (m_lastUnusedCheck.QuadPart == 0) {
        m_lastUnusedCheck = currentTime;
    } else {
        double elapsed = static_cast<double>(currentTime.QuadPart - m_lastUnusedCheck.QuadPart) / 
                         static_cast<double>(frequency.QuadPart);
        
        m_unusedTime += elapsed;
        m_lastUnusedCheck = currentTime;
        
        // Clean up unused buffers after timeout
        if (m_unusedTime > UNUSED_TIMEOUT_SECONDS && !m_bufferPool.empty()) {
            // Keep only the largest buffer and remove the rest
            auto largestIt = std::max_element(m_bufferPool.begin(), m_bufferPool.end(),
                [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                    return a.size() < b.size();
                });
            
            std::vector<uint8_t> largestBuffer;
            std::swap(largestBuffer, *largestIt);
            
            m_bufferPool.clear();
            m_bufferPool.push_back(std::move(largestBuffer));
            
            // Reset unused time
            m_unusedTime = 0;
        }
    }
}

// Replace OptimizedCopyFrame method with xsimd version
void ScreenCapture::OptimizedCopyFrame(uint8_t* dst, const uint8_t* src, 
                                     int width, int height, LONG srcStride) {
    // Use our SIMD optimized copy function
    SimdMemUtils::CopyRowWithStride(dst, src, width, height, srcStride);
} 
