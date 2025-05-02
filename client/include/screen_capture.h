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
    
    // Resource clean-up methods
    void CleanupDXGI();
    void CleanupGDI();
}; 