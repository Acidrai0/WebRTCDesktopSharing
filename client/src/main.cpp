#include "screen_capture.h"
#include "encoder.h"
#include "webrtc_session.h"
#include "preview_window.h"
#include "performance_logger.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <Windows.h>

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
    std::string logFile = "performance_double_buffered.csv";
    bool enableLogging = false;
    std::string bufferingMode = "double"; // Default to double buffering
    
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
        } else if (arg == "--log-file" && i + 1 < argc) {
            logFile = argv[++i];
            enableLogging = true;
        } else if (arg == "--enable-logging") {
            enableLogging = true;
        } else if (arg == "--buffering-mode" && i + 1 < argc) {
            bufferingMode = argv[++i];
            if (bufferingMode != "single" && bufferingMode != "double" && bufferingMode != "triple") {
                std::cerr << "Invalid buffering mode: " << bufferingMode << ". Using default (double)." << std::endl;
                bufferingMode = "double";
            }
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
            std::cout << "  --buffering-mode <mode>   Buffering mode: single, double, or triple (default: double)" << std::endl;
            std::cout << "  --enable-logging          Enable performance logging" << std::endl;
            std::cout << "  --log-file <filename>     Log file name (default: performance_double_buffered.csv)" << std::endl;
            std::cout << "  --help                    Show this help message" << std::endl;
            return 0;
        }
    }
    
    std::cout << "Starting desktop sharing client" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
    std::cout << "Using " << bufferingMode << " buffering mode" << std::endl;
    
    // Initialize performance logger if enabled
    if (enableLogging) {
        if (!PerformanceLogger::GetInstance().Initialize(logFile)) {
            std::cerr << "Failed to initialize performance logger" << std::endl;
            enableLogging = false;
        } else {
            std::cout << "Performance logging enabled to " << logFile << std::endl;
        }
    }
    
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Initialize screen capture
    ScreenCapture screenCapture;
    if (!screenCapture.Initialize(monitorIndex)) {
        std::cerr << "Failed to initialize screen capture" << std::endl;
        return 1;
    }
    
    // Configure buffering mode
    if (bufferingMode == "single") {
        screenCapture.SetBufferingMode(ScreenCapture::BufferingMode::Single);
    } else if (bufferingMode == "double") {
        screenCapture.SetBufferingMode(ScreenCapture::BufferingMode::Double);
    } else if (bufferingMode == "triple") {
        screenCapture.SetBufferingMode(ScreenCapture::BufferingMode::Triple);
    }
    
    // Get initial frame to determine dimensions
    std::vector<uint8_t> frameBuffer;
    int width = 0, height = 0;
    if (!screenCapture.CaptureFrame(frameBuffer, width, height)) {
        std::cerr << "Failed to capture initial frame" << std::endl;
        return 1;
    }
    
    std::cout << "Captured screen with dimensions: " << width << "x" << height << std::endl;
    
    // Initialize encoder (which now handles frame rate control in a separate thread)
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
    
    // Tracking statistics for display
    LARGE_INTEGER frequency, statsLastTime;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&statsLastTime);
    unsigned frameCount = 0;
    unsigned totalFrameCount = 0;
    float lastEncoderFps = 0.0f;
    int lastQueueSize = 0;
    
    // Register callback to get encoder stats
    if (enableLogging) {
        encoder.SetStatsCallback([&lastEncoderFps, &lastQueueSize](float fps, int queueSize) {
            lastEncoderFps = fps;
            lastQueueSize = queueSize;
        });
    }
    
    std::cout << "Starting capture loop..." << std::endl;
    
    // Main capture loop is now simpler - no need to manage timing for encoding
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
            
            // Send frame to encoder (which now handles timing in a separate thread)
            encoder.EncodeFrame(frameBuffer, width, height);
            
            // Update statistics
            frameCount++;
            totalFrameCount++;
            
            // Print statistics every second
            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            LONGLONG elapsed = currentTime.QuadPart - statsLastTime.QuadPart;
            
            if (elapsed > frequency.QuadPart) {  // 1 second interval
                float captureRate = (float)(frameCount * frequency.QuadPart) / elapsed;
                float captureFps = screenCapture.GetFrameRate();
                
                std::cout << "Capture rate: " << captureRate << " fps, Capture FPS: " << captureFps << std::endl;
                
                // Log performance data
                if (enableLogging) {
                    PerformanceLogger::GetInstance().LogPerformance("Capture", captureFps, totalFrameCount);
                    PerformanceLogger::GetInstance().LogPerformance("Main Loop", captureRate, totalFrameCount);
                    PerformanceLogger::GetInstance().LogPerformance("Encoder", lastEncoderFps, totalFrameCount, lastQueueSize);
                }
                
                frameCount = 0;
                statsLastTime = currentTime;
            }
        } else {
            std::cerr << "Frame capture failed" << std::endl;
            
            // Small delay on failure to avoid CPU spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Slight delay to avoid maxing out CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "Shutting down..." << std::endl;
    
    // Shut down the performance logger
    if (enableLogging) {
        PerformanceLogger::GetInstance().Shutdown();
    }
    
    // Clean up resources
    if (previewWindow) {
        previewWindow->Shutdown();
    }
    
    return 0;
} 