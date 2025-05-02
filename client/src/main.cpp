#include "screen_capture.h"
#include "encoder.h"
#include "webrtc_session.h"
#include "preview_window.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>

// Global flag to control the main loop
std::atomic<bool> g_running = true;

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    std::cout << "Received signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    std::string signalingServer = "ws://localhost:8080";
    int monitorIndex = 0;
    int fps = 30;
    int bitrate = 2000000;
    bool showPreview = true;
    float previewScale = 1.0f;  // Changed from 0.75f to 1.0f for pixel-perfect display
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--signaling-server" && i + 1 < argc) {
            signalingServer = argv[++i];
        } else if (arg == "--monitor" && i + 1 < argc) {
            monitorIndex = std::stoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = std::stoi(argv[++i]);
        } else if (arg == "--bitrate" && i + 1 < argc) {
            bitrate = std::stoi(argv[++i]);
        } else if (arg == "--no-preview") {
            showPreview = false;
        } else if (arg == "--preview-scale" && i + 1 < argc) {
            previewScale = std::stof(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "Desktop Sharing Client" << std::endl;
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --signaling-server <url>  Signaling server URL (default: ws://localhost:8080)" << std::endl;
            std::cout << "  --monitor <index>         Monitor index to capture (default: 0)" << std::endl;
            std::cout << "  --fps <fps>               Target FPS (default: 30)" << std::endl;
            std::cout << "  --bitrate <bitrate>       Target bitrate in bps (default: 2000000)" << std::endl;
            std::cout << "  --no-preview              Disable preview window" << std::endl;
            std::cout << "  --preview-scale <scale>   Scale preview window (default: 1.0)" << std::endl;
            std::cout << "  --help                    Show this help message" << std::endl;
            return 0;
        }
    }
    
    std::cout << "Starting desktop sharing client" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
    
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Initialize screen capture
    ScreenCapture screenCapture;
    if (!screenCapture.Initialize(monitorIndex)) {
        std::cerr << "Failed to initialize screen capture" << std::endl;
        return 1;
    }
    
    // Get initial frame to determine dimensions
    std::vector<uint8_t> frameBuffer;
    int width = 0, height = 0;
    if (!screenCapture.CaptureFrame(frameBuffer, width, height)) {
        std::cerr << "Failed to capture initial frame" << std::endl;
        return 1;
    }
    
    std::cout << "Captured screen with dimensions: " << width << "x" << height << std::endl;
    
    // Initialize encoder
    Encoder encoder;
    if (!encoder.Initialize(width, height, fps, bitrate)) {
        std::cerr << "Failed to initialize encoder" << std::endl;
        return 1;
    }
    
    // Initialize preview window if enabled
    std::unique_ptr<PreviewWindow> previewWindow;
    if (showPreview) {
        previewWindow = std::make_unique<PreviewWindow>();
        
        // Calculate scaled dimensions
        int previewWidth = static_cast<int>(width * previewScale);
        int previewHeight = static_cast<int>(height * previewScale);
        
        if (!previewWindow->Initialize("Screen Capture Preview", previewWidth, previewHeight)) {
            std::cerr << "Failed to initialize preview window, continuing without preview" << std::endl;
            previewWindow.reset();
        }
    }
    
    // Main capture loop
    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto frameInterval = std::chrono::milliseconds(1000 / fps);
    
    std::cout << "Starting capture loop..." << std::endl;
    
    while (g_running) {
        // Capture frame
        if (screenCapture.CaptureFrame(frameBuffer, width, height)) {
            // Update preview window if enabled
            if (previewWindow) {
                if (!previewWindow->UpdateFrame(frameBuffer, width, height)) {
                    std::cerr << "Failed to update preview frame" << std::endl;
                }
                
                // Process window messages
                if (!previewWindow->ProcessMessages()) {
                    std::cout << "Preview window closed, shutting down..." << std::endl;
                    g_running = false;
                    break;
                }
            }
            
            // Encode frame
            encoder.EncodeFrame(frameBuffer, width, height);
        } else {
            std::cerr << "Frame capture failed" << std::endl;
        }
        
        // Calculate time until next frame
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime);
        auto sleepTime = frameInterval - elapsed;
        
        if (sleepTime > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleepTime);
        }
        
        lastFrameTime = std::chrono::high_resolution_clock::now();
    }
    
    std::cout << "Shutting down..." << std::endl;
    
    // Clean up resources
    if (previewWindow) {
        previewWindow->Shutdown();
    }
    
    return 0;
} 