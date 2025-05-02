#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

class PreviewWindow {
public:
    PreviewWindow();
    ~PreviewWindow();

    bool Initialize(const std::string& title, int width, int height);
    void Shutdown();
    
    // Update the display with new frame data
    bool UpdateFrame(const std::vector<uint8_t>& frameData, int width, int height);
    
    // Process window messages
    bool ProcessMessages();
    
    // Get window handle
    HWND GetWindowHandle() const { return m_hwnd; }
    
private:
    // Window properties
    HWND m_hwnd;
    HINSTANCE m_hInstance;
    std::string m_windowTitle;
    int m_width;
    int m_height;
    
    // D3D11 objects
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_deviceContext;
    IDXGISwapChain* m_swapChain;
    ID3D11RenderTargetView* m_renderTargetView;
    ID3D11Texture2D* m_texture;
    ID3D11ShaderResourceView* m_textureView;
    
    // Shader objects
    ID3D11VertexShader* m_vertexShader;
    ID3D11PixelShader* m_pixelShader;
    ID3D11SamplerState* m_samplerState;
    ID3D11Buffer* m_vertexBuffer;
    ID3D11Buffer* m_indexBuffer;
    ID3D11InputLayout* m_inputLayout;
    
    // Current frame data
    std::vector<uint8_t> m_frameBuffer;
    std::mutex m_frameBufferMutex;
    bool m_hasNewFrame;
    
    // Window procedure
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    
    // DirectX initialization
    bool InitializeDirectX();
    void CleanupDirectX();
    bool CreateShaders();
    bool CreateGeometry();
    
    // Rendering
    void Render();
    bool CreateTexture();
}; 