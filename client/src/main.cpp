#include "screen_capture.h"
#include "encoder.h"
#include "webrtc_session.h"
#include "performance_logger.h"
#ifndef NO_PREVIEW_WINDOW
#include "preview_window.h"
#endif

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

// Add a new command line option for the ring buffer system
bool parseCommandLine(int argc, char* argv[], bool& showPreview, float& previewScale, 
                      bool& fullscreen, bool& enableLogging, std::string& logFile, 
                      std::string& testName, bool& noPreview, int& benchmarkFrames,
                      bool& useRingBuffer) {
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            std::cout << "Desktop Sharing Client" << std::endl;
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --no-preview             Disable preview window" << std::endl;
            std::cout << "  --preview-scale <scale>  Set preview window scale (default: 0.75)" << std::endl;
            std::cout << "  --fullscreen             Show preview window in fullscreen mode" << std::endl;
            std::cout << "  --enable-logging         Enable performance logging" << std::endl;
            std::cout << "  --log-file <filename>    Specify log file (default: perf_log.csv)" << std::endl;
            std::cout << "  --test-name <name>       Add a test name to log file" << std::endl;
            std::cout << "  --benchmark <frames>     Run benchmark mode for specified frames" << std::endl;
            std::cout << "  --use-ring-buffer        Use ring buffer system (for testing)" << std::endl;
            std::cout << "  --help, -h               Show this help message" << std::endl;
            return false;
        } else if (arg == "--no-preview") {
            noPreview = true;
            showPreview = false;
        } else if (arg == "--preview-scale") {
            if (i + 1 < argc) {
                previewScale = std::stof(argv[++i]);
            }
        } else if (arg == "--fullscreen") {
            fullscreen = true;
        } else if (arg == "--enable-logging") {
            enableLogging = true;
        } else if (arg == "--log-file") {
            if (i + 1 < argc) {
                logFile = argv[++i];
            }
        } else if (arg == "--test-name") {
            if (i + 1 < argc) {
                testName = argv[++i];
            }
        } else if (arg == "--benchmark") {
            if (i + 1 < argc) {
                benchmarkFrames = std::stoi(argv[++i]);
            }
        } else if (arg == "--use-ring-buffer") {
            useRingBuffer = true;
        }
    }
    
    return true;
}

int main(int argc, char* argv[]) {
    // Default settings
    bool showPreview = true;
    float previewScale = 1.0f;  // Changed from 0.75f to 1.0f for pixel-perfect display
    bool fullscreen = false;
    bool enableLogging = false;
    std::string logFile = "perf_log.csv";
    std::string testName = "";
    bool noPreview = false;
    int benchmarkFrames = 0;
    bool useRingBuffer = false;  // Default to original queue system
    
    // Parse command line
    if (!parseCommandLine(argc, argv, showPreview, previewScale, fullscreen, enableLogging,
                         logFile, testName, noPreview, benchmarkFrames, useRingBuffer)) {
        return 0;
    }
    
    std::cout << "Starting desktop sharing client" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
    std::cout << "Using double buffering mode" << std::endl;
    
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
    if (!screenCapture.Initialize(0)) {
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
    
    // Configure encoder
    Encoder encoder;
    encoder.SetPipelineMode(EncoderPipelineMode::Parallel);
    
    // Set buffer system mode based on command line
    if (useRingBuffer) {
        encoder.SetBufferSystemMode(BufferSystemMode::RingBuffer);
        std::cout << "Using ring buffer system for improved timing control" << std::endl;
    }
    
    // Rest of initialization
    encoder.Initialize(width, height, 30, 5000, testName);
    
    // Initialize preview window if enabled
#ifndef NO_PREVIEW_WINDOW
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
#endif
    
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
    while (g_running && (benchmarkFrames == 0 || totalFrameCount < benchmarkFrames)) {
        // Capture frame
        if (screenCapture.CaptureFrame(frameBuffer, width, height)) {
            // Debug output to check frameBuffer size
            std::cout << "Frame captured - Buffer size: " << frameBuffer.size() 
                      << " bytes, Dimensions: " << width << "x" << height 
                      << ", Expected size: " << (width * height * 4) << " bytes" << std::endl;
            
            // Ensure the frame buffer has valid data before proceeding
            if (frameBuffer.size() >= width * height * 4) {
                // Update preview window if enabled
#ifndef NO_PREVIEW_WINDOW
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
#endif
                
                // Use zero-copy or regular encoding based on configuration
                if (useRingBuffer) {
                    // Create a shared pointer to the frame buffer
                    auto sharedFrame = std::make_shared<std::vector<uint8_t>>(frameBuffer);
                    
                    // Send frame to encoder using zero-copy method
                    encoder.EncodeFrameZeroCopy(sharedFrame, width, height);
                } else {
                    // Make a deep copy of the frame buffer to ensure it stays valid for the encoder
                    std::vector<uint8_t> encoderFrame(frameBuffer);
                    
                    // Send frame to encoder (which now handles timing in a separate thread)
                    encoder.EncodeFrame(encoderFrame, width, height);
                }
                
                // Update statistics
                frameCount++;
                totalFrameCount++;
            } else {
                std::cerr << "Skipping frame with invalid buffer size" << std::endl;
            }
            
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
#ifndef NO_PREVIEW_WINDOW
    if (previewWindow) {
        previewWindow->Shutdown();
    }
#endif
    
    return 0;
} 