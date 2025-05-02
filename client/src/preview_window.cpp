#include "preview_window.h"
#include <iostream>

// Store the class instance pointer for the window procedure
PreviewWindow* g_previewWindowInstance = nullptr;

// Vertex structure for rendering a quad
struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 texCoord;
};

PreviewWindow::PreviewWindow() 
    : m_hwnd(nullptr)
    , m_hInstance(GetModuleHandle(nullptr))
    , m_width(800)
    , m_height(600)
    , m_device(nullptr)
    , m_deviceContext(nullptr)
    , m_swapChain(nullptr)
    , m_renderTargetView(nullptr)
    , m_texture(nullptr)
    , m_textureView(nullptr)
    , m_vertexShader(nullptr)
    , m_pixelShader(nullptr)
    , m_samplerState(nullptr)
    , m_vertexBuffer(nullptr)
    , m_indexBuffer(nullptr)
    , m_inputLayout(nullptr)
    , m_hasNewFrame(false)
{
    g_previewWindowInstance = this;
}

PreviewWindow::~PreviewWindow() {
    Shutdown();
    g_previewWindowInstance = nullptr;
}

bool PreviewWindow::Initialize(const std::string& title, int width, int height) {
    m_windowTitle = title;
    m_width = width;
    m_height = height;
    
    // Register the window class
    const char* CLASS_NAME = "PreviewWindowClass";
    
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    
    RegisterClassA(&wc);
    
    // Calculate window size based on client area
    RECT windowRect = {0, 0, m_width, m_height};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
    
    // Create the window
    m_hwnd = CreateWindowExA(
        0,                              // Optional window styles
        CLASS_NAME,                     // Window class
        m_windowTitle.c_str(),          // Window text
        WS_OVERLAPPEDWINDOW,            // Window style
        CW_USEDEFAULT, CW_USEDEFAULT,   // Position
        windowRect.right - windowRect.left,    // Width
        windowRect.bottom - windowRect.top,    // Height
        nullptr,                        // Parent window    
        nullptr,                        // Menu
        m_hInstance,                    // Instance handle
        nullptr                         // Additional application data
    );
    
    if (m_hwnd == nullptr) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }
    
    // Initialize DirectX
    if (!InitializeDirectX()) {
        std::cerr << "Failed to initialize DirectX" << std::endl;
        return false;
    }
    
    // Create shaders
    if (!CreateShaders()) {
        std::cerr << "Failed to create shaders" << std::endl;
        return false;
    }
    
    // Create geometry
    if (!CreateGeometry()) {
        std::cerr << "Failed to create geometry" << std::endl;
        return false;
    }
    
    // Show the window
    ShowWindow(m_hwnd, SW_SHOW);
    
    return true;
}

void PreviewWindow::Shutdown() {
    CleanupDirectX();
    
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool PreviewWindow::ProcessMessages() {
    MSG msg = {};
    
    // Process all messages in the queue
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Render the current frame
    Render();
    
    return true;
}

bool PreviewWindow::UpdateFrame(const std::vector<uint8_t>& frameData, int width, int height) {
    if (frameData.empty() || width <= 0 || height <= 0) {
        return false;
    }
    
    // Check if we need to recreate the texture due to size change
    if (width != m_width || height != m_height) {
        m_width = width;
        m_height = height;
        
        // Recreate the texture
        if (m_texture) {
            m_texture->Release();
            m_texture = nullptr;
        }
        
        if (m_textureView) {
            m_textureView->Release();
            m_textureView = nullptr;
        }
        
        // Resize the window to match the new dimensions
        RECT windowRect = {0, 0, m_width, m_height};
        AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 
                     windowRect.right - windowRect.left, 
                     windowRect.bottom - windowRect.top, 
                     SWP_NOMOVE | SWP_NOZORDER);
        
        // Create a new texture
        if (!CreateTexture()) {
            std::cerr << "Failed to create texture for new frame size" << std::endl;
            return false;
        }
    }
    
    // Update the texture with new frame data
    if (m_device && m_deviceContext && m_texture) {
        // Lock the frame buffer for writing
        {
            std::lock_guard<std::mutex> lock(m_frameBufferMutex);
            
            // Copy the frame data
            m_frameBuffer = frameData;
            m_hasNewFrame = true;
        }
        
        return true;
    }
    
    return false;
}

bool PreviewWindow::InitializeDirectX() {
    // Create the device and swap chain
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = m_width;
    swapChainDesc.BufferDesc.Height = m_height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = m_hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                    // Adapter
        D3D_DRIVER_TYPE_HARDWARE,   // Driver type
        nullptr,                    // Software module
        0,                          // Flags
        featureLevels,              // Feature levels
        1,                          // Feature level count
        D3D11_SDK_VERSION,          // SDK version
        &swapChainDesc,             // Swap chain description
        &m_swapChain,               // Swap chain
        &m_device,                  // Device
        &featureLevel,              // Feature level
        &m_deviceContext            // Device context
    );
    
    if (FAILED(hr)) {
        std::cerr << "Failed to create device and swap chain: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Create the render target view
    ID3D11Texture2D* backBuffer = nullptr;
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    
    if (FAILED(hr)) {
        std::cerr << "Failed to get back buffer: " << std::hex << hr << std::endl;
        return false;
    }
    
    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_renderTargetView);
    backBuffer->Release();
    
    if (FAILED(hr)) {
        std::cerr << "Failed to create render target view: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Set the render target
    m_deviceContext->OMSetRenderTargets(1, &m_renderTargetView, nullptr);
    
    // Set up the viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    
    m_deviceContext->RSSetViewports(1, &viewport);
    
    // Create sampler state
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    
    hr = m_device->CreateSamplerState(&samplerDesc, &m_samplerState);
    if (FAILED(hr)) {
        std::cerr << "Failed to create sampler state: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Create the texture for the frame data
    if (!CreateTexture()) {
        std::cerr << "Failed to create initial texture" << std::endl;
        return false;
    }
    
    return true;
}

bool PreviewWindow::CreateShaders() {
    // Vertex shader code
    const char* vsCode = 
        "struct VS_INPUT {\n"
        "    float3 Pos : POSITION;\n"
        "    float2 Tex : TEXCOORD;\n"
        "};\n"
        "struct PS_INPUT {\n"
        "    float4 Pos : SV_POSITION;\n"
        "    float2 Tex : TEXCOORD;\n"
        "};\n"
        "PS_INPUT main(VS_INPUT input) {\n"
        "    PS_INPUT output = (PS_INPUT)0;\n"
        "    output.Pos = float4(input.Pos, 1.0f);\n"
        "    output.Tex = input.Tex;\n"
        "    return output;\n"
        "}\n";
    
    // Pixel shader code
    const char* psCode =
        "Texture2D txDiffuse : register(t0);\n"
        "SamplerState samLinear : register(s0);\n"
        "struct PS_INPUT {\n"
        "    float4 Pos : SV_POSITION;\n"
        "    float2 Tex : TEXCOORD;\n"
        "};\n"
        "float4 main(PS_INPUT input) : SV_Target {\n"
        "    return txDiffuse.Sample(samLinear, input.Tex);\n"
        "}\n";
    
    HRESULT hr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    
    // Compile vertex shader
    hr = D3DCompile(vsCode, strlen(vsCode), "VS", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Vertex shader compilation failed: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            errorBlob->Release();
        }
        return false;
    }
    
    // Create vertex shader
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (FAILED(hr)) {
        vsBlob->Release();
        std::cerr << "Failed to create vertex shader: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Define input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    
    // Create input layout
    hr = m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
    vsBlob->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to create input layout: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Compile pixel shader
    hr = D3DCompile(psCode, strlen(psCode), "PS", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Pixel shader compilation failed: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            errorBlob->Release();
        }
        return false;
    }
    
    // Create pixel shader
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to create pixel shader: " << std::hex << hr << std::endl;
        return false;
    }
    
    return true;
}

bool PreviewWindow::CreateGeometry() {
    // Create a quad with texture coordinates
    Vertex vertices[] = {
        { DirectX::XMFLOAT3(-1.0f, -1.0f, 0.0f), DirectX::XMFLOAT2(0.0f, 1.0f) },
        { DirectX::XMFLOAT3(-1.0f,  1.0f, 0.0f), DirectX::XMFLOAT2(0.0f, 0.0f) },
        { DirectX::XMFLOAT3( 1.0f,  1.0f, 0.0f), DirectX::XMFLOAT2(1.0f, 0.0f) },
        { DirectX::XMFLOAT3( 1.0f, -1.0f, 0.0f), DirectX::XMFLOAT2(1.0f, 1.0f) }
    };
    
    // Create vertex buffer
    D3D11_BUFFER_DESC vertexBufferDesc = {};
    vertexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    vertexBufferDesc.ByteWidth = sizeof(Vertex) * 4;
    vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA vertexData = {};
    vertexData.pSysMem = vertices;
    
    HRESULT hr = m_device->CreateBuffer(&vertexBufferDesc, &vertexData, &m_vertexBuffer);
    if (FAILED(hr)) {
        std::cerr << "Failed to create vertex buffer: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Create index buffer (two triangles)
    DWORD indices[] = {
        0, 1, 2,
        0, 2, 3
    };
    
    D3D11_BUFFER_DESC indexBufferDesc = {};
    indexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    indexBufferDesc.ByteWidth = sizeof(DWORD) * 6;
    indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA indexData = {};
    indexData.pSysMem = indices;
    
    hr = m_device->CreateBuffer(&indexBufferDesc, &indexData, &m_indexBuffer);
    if (FAILED(hr)) {
        std::cerr << "Failed to create index buffer: " << std::hex << hr << std::endl;
        return false;
    }
    
    return true;
}

bool PreviewWindow::CreateTexture() {
    if (!m_device) {
        return false;
    }
    
    // Create the texture description
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = m_width;
    textureDesc.Height = m_height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // BGRA format
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DYNAMIC;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, &m_texture);
    
    if (FAILED(hr)) {
        std::cerr << "Failed to create texture: " << std::hex << hr << std::endl;
        return false;
    }
    
    // Create the shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    
    hr = m_device->CreateShaderResourceView(m_texture, &srvDesc, &m_textureView);
    
    if (FAILED(hr)) {
        std::cerr << "Failed to create shader resource view: " << std::hex << hr << std::endl;
        m_texture->Release();
        m_texture = nullptr;
        return false;
    }
    
    return true;
}

void PreviewWindow::Render() {
    if (!m_deviceContext || !m_swapChain || !m_texture) {
        return;
    }
    
    // Check if we have a new frame to render
    bool hasFrame = false;
    {
        std::lock_guard<std::mutex> lock(m_frameBufferMutex);
        hasFrame = m_hasNewFrame && !m_frameBuffer.empty();
    }
    
    if (hasFrame) {
        // Map the texture for writing
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT hr = m_deviceContext->Map(m_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        
        if (SUCCEEDED(hr)) {
            // Copy the frame data to the texture
            std::lock_guard<std::mutex> lock(m_frameBufferMutex);
            
            const size_t bytesPerPixel = 4;  // BGRA format
            const size_t frameWidth = m_width;
            const size_t frameHeight = m_height;
            
            uint8_t* dst = static_cast<uint8_t*>(mappedResource.pData);
            const uint8_t* src = m_frameBuffer.data();
            
            for (size_t y = 0; y < frameHeight; ++y) {
                memcpy(dst, src, frameWidth * bytesPerPixel);
                dst += mappedResource.RowPitch;
                src += frameWidth * bytesPerPixel;
            }
            
            m_deviceContext->Unmap(m_texture, 0);
            m_hasNewFrame = false;
        }
    }
    
    // Clear the render target
    static const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_deviceContext->ClearRenderTargetView(m_renderTargetView, clearColor);
    
    // Set up the rendering pipeline
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_deviceContext->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
    m_deviceContext->IASetIndexBuffer(m_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->IASetInputLayout(m_inputLayout);
    
    // Set shaders
    m_deviceContext->VSSetShader(m_vertexShader, nullptr, 0);
    m_deviceContext->PSSetShader(m_pixelShader, nullptr, 0);
    
    // Set texture and sampler
    m_deviceContext->PSSetShaderResources(0, 1, &m_textureView);
    m_deviceContext->PSSetSamplers(0, 1, &m_samplerState);
    
    // Draw the quad (two triangles)
    m_deviceContext->DrawIndexed(6, 0, 0);
    
    // Present the back buffer to the screen
    m_swapChain->Present(1, 0);
}

void PreviewWindow::CleanupDirectX() {
    if (m_inputLayout) {
        m_inputLayout->Release();
        m_inputLayout = nullptr;
    }
    
    if (m_indexBuffer) {
        m_indexBuffer->Release();
        m_indexBuffer = nullptr;
    }
    
    if (m_vertexBuffer) {
        m_vertexBuffer->Release();
        m_vertexBuffer = nullptr;
    }
    
    if (m_samplerState) {
        m_samplerState->Release();
        m_samplerState = nullptr;
    }
    
    if (m_pixelShader) {
        m_pixelShader->Release();
        m_pixelShader = nullptr;
    }
    
    if (m_vertexShader) {
        m_vertexShader->Release();
        m_vertexShader = nullptr;
    }
    
    if (m_textureView) {
        m_textureView->Release();
        m_textureView = nullptr;
    }
    
    if (m_texture) {
        m_texture->Release();
        m_texture = nullptr;
    }
    
    if (m_renderTargetView) {
        m_renderTargetView->Release();
        m_renderTargetView = nullptr;
    }
    
    if (m_swapChain) {
        m_swapChain->Release();
        m_swapChain = nullptr;
    }
    
    if (m_deviceContext) {
        m_deviceContext->Release();
        m_deviceContext = nullptr;
    }
    
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
}

LRESULT CALLBACK PreviewWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
} 