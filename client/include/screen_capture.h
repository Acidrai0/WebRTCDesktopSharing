#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <vector>
#include <Windows.h>

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    bool Initialize(int monitorIndex = 0);
    bool CaptureFrame(std::vector<uint8_t>& outputBuffer, int& width, int& height);
    float GetFrameRate() const;
    
private:
    bool InitializeDXGI();
    bool FallbackToGDI();
    bool CaptureDXGI(std::vector<uint8_t>& outputBuffer, int& width, int& height);
    bool CaptureGDI(std::vector<uint8_t>& outputBuffer, int& width, int& height);
    
    // Mouse cursor rendering
    void RenderCursorToFrame(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight);
    bool GetCursorInfo(POINT& position, HCURSOR& cursor);
    void DrawCursor(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                   const POINT& position, HCURSOR cursor);
    void DrawIBeamCursor(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                        const POINT& position);
    void DrawCursorPixel(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                        int x, int y, int bytesPerPixel, uint8_t r, uint8_t g, uint8_t b);
    
    // DXGI components
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIOutputDuplication* m_dxgiOutputDuplication = nullptr;
    ID3D11Texture2D* m_stagingTexture = nullptr;     // For CPU access to the frame
    ID3D11Texture2D* m_acquiredDesktopImage = nullptr; // Last acquired desktop image
    
    // DXGI cursor info
    POINT m_cursorPosition = {0, 0};             // Current cursor position
    bool m_cursorVisible = false;                // Is the cursor currently visible?
    LARGE_INTEGER m_lastCursorUpdateTime = {0};  // When was the cursor last updated?
    
    // GDI components
    HDC m_hdcScreen = nullptr;
    HDC m_hdcMemory = nullptr;
    HBITMAP m_hBitmap = nullptr;
    
    // Monitor info
    int m_monitorIndex = 0;
    int m_monitorWidth = 0;
    int m_monitorHeight = 0;
    RECT m_monitorRect = {0};
    
    // Capture state
    bool m_usingDXGI = true;
    bool m_captureCursor = true;
    
    // Performance tracking
    LARGE_INTEGER m_lastCaptureTime = {0};
    int m_frameCount = 0;
    float m_framerate = 0.0f;
    
    // Resource clean-up methods
    void CleanupDXGI();
    void CleanupGDI();
}; 