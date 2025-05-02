#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <vector>
#include <cstdint>
#include <mfapi.h> // Media Foundation for optimized memory operations

// Memory alignment constant - align to 64 bytes for optimal cache line performance
#define MEMORY_ALIGNMENT 64

class ScreenCapture {
public:
    // Buffering mode enum
    enum class BufferingMode {
        Double
    };
    
    ScreenCapture();
    ~ScreenCapture();
    
    bool Initialize(int monitorIndex = 0);
    bool CaptureFrame(std::vector<uint8_t>& outputBuffer, int& width, int& height);
    float GetFrameRate() const;
    
    // Set the buffering mode (double)
    void SetBufferingMode(BufferingMode mode);
    
private:
    bool InitializeDXGI();
    bool FallbackToGDI();
    bool CaptureDXGI(std::vector<uint8_t>& outputBuffer, int& width, int& height);
    bool CaptureGDI(std::vector<uint8_t>& outputBuffer, int& width, int& height);
    void CleanupDXGI();
    void CleanupGDI();
    
    // Memory alignment helpers
    size_t GetAlignedSize(size_t size) const;
    uint8_t* AlignBuffer(std::vector<uint8_t>& buffer, size_t requiredSize);
    
    // Optimized memory copy functions
    void OptimizedCopyFrame(uint8_t* dst, const uint8_t* src, 
                           int width, int height, LONG srcStride);
    
    // Cursor rendering methods
    void RenderCursorToFrame(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight);
    bool GetCursorInfo(POINT& position, HCURSOR& cursor);
    void DrawCursor(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                   const POINT& position, HCURSOR cursor);
    void DrawIBeamCursor(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                        const POINT& position);
    void DrawCursorPixel(std::vector<uint8_t>& frameData, int frameWidth, int frameHeight, 
                        int x, int y, int bytesPerPixel, uint8_t r, uint8_t g, uint8_t b);
    
    // DXGI related members
    ID3D11Device* m_d3dDevice;
    ID3D11DeviceContext* m_d3dContext;
    IDXGIOutputDuplication* m_dxgiOutputDuplication;
    ID3D11Texture2D* m_acquiredDesktopImage;
    ID3D11Texture2D* m_stagingTextures[2]; // Double buffering only
    int m_currentTextureIndex;
    BufferingMode m_bufferingMode;         // Current buffering mode
    
    // Memory alignment related members
    size_t m_alignedBufferPadding;         // Extra padding for alignment
    
    // GDI related members
    HDC m_hdcScreen;
    HDC m_hdcMemory;
    HBITMAP m_hBitmap;
    
    // Monitor information
    int m_monitorIndex;
    int m_monitorWidth;
    int m_monitorHeight;
    RECT m_monitorRect;
    
    // Cursor information
    bool m_captureCursor;
    bool m_cursorVisible;
    POINT m_cursorPosition;
    LARGE_INTEGER m_lastCursorUpdateTime;
    
    // Performance tracking
    LARGE_INTEGER m_lastCaptureTime;
    unsigned int m_frameCount;
    float m_framerate;
    
    // Capture flags
    bool m_usingDXGI;
}; 