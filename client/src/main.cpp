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
    int benchmarkFrames = 0; // Number of frames to capture for benchmarking (0 = unlimited)
    std::string testName = ""; // Test name for including in output filenames
    std::string pipelineMode = "sequential"; // Default to sequential pipeline
    bool useZeroCopy = true; // Default to zero-copy mode for better performance
    
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
        } else if (arg == "--pipeline-mode" && i + 1 < argc) {
            pipelineMode = argv[++i];
        } else if (arg == "--zero-copy" && i + 1 < argc) {
            std::string value = argv[++i];
            useZeroCopy = (value == "true" || value == "1");
        } else if (arg == "--preview-window" && i + 1 < argc) {
            std::string value = argv[++i];
            showPreview = (value == "true");
        } else if (arg == "--no-preview") {
            showPreview = false;
        } else if (arg == "--preview-scale" && i + 1 < argc) {
            previewScale = std::stof(argv[++i]);
        } else if (arg == "--log-file" && i + 1 < argc) {
            logFile = argv[++i];
            enableLogging = true;
        } else if (arg == "--enable-logging") {
            enableLogging = true;
        } else if (arg == "--benchmark" && i + 1 < argc) {
            benchmarkFrames = std::stoi(argv[++i]);
        } else if (arg == "--output-csv" && i + 1 < argc) {
            logFile = argv[++i];
            enableLogging = true;
        } else if (arg == "--test-name" && i + 1 < argc) {
            testName = argv[++i];
            std::cout << "Using test name: " << testName << std::endl;
        } else if (arg == "--help") {
            std::cout << "Desktop Sharing Client" << std::endl;
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --signaling-server <url>  Signaling server URL (default: ws://localhost:8080)" << std::endl;
            std::cout << "  --monitor <index>         Monitor index to capture (default: 0)" << std::endl;
            std::cout << "  --fps <fps>               Target FPS (default: 30)" << std::endl;
            std::cout << "  --bitrate <bitrate>       Target bitrate in bps (default: 2000000)" << std::endl;
            std::cout << "  --pipeline-mode <mode>   Encoding pipeline mode: sequential or parallel (default: sequential)" << std::endl;
            std::cout << "  --zero-copy <bool>        Enable or disable zero-copy mode (default: true)" << std::endl;
            std::cout << "  --preview-window <bool>   Enable or disable preview window" << std::endl;
            std::cout << "  --no-preview              Disable preview window" << std::endl;
            std::cout << "  --preview-scale <scale>   Scale preview window (default: 1.0)" << std::endl;
            std::cout << "  --enable-logging          Enable performance logging" << std::endl;
            std::cout << "  --log-file <filename>     Log file name (default: performance_double_buffered.csv)" << std::endl;
            std::cout << "  --benchmark <frames>      Run in benchmark mode for specified number of frames" << std::endl;
            std::cout << "  --output-csv <filename>   Output CSV file for benchmark results" << std::endl;
            std::cout << "  --test-name <name>        Test name to include in output H.264 files" << std::endl;
            std::cout << "  --help                    Show this help message" << std::endl;
            return 0;
        }
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
    
    // Initialize encoder (which now handles frame rate control in a separate thread)
    Encoder encoder;
    if (!encoder.Initialize(width, height, fps, bitrate, testName)) {
        std::cerr << "Failed to initialize encoder" << std::endl;
        return 1;
    }
    
    // Set pipeline mode if specified
    if (pipelineMode == "parallel") {
        std::cout << "Using parallel encoding pipeline" << std::endl;
        encoder.SetPipelineMode(EncoderPipelineMode::Parallel);
    } else {
        std::cout << "Using sequential encoding pipeline" << std::endl;
        encoder.SetPipelineMode(EncoderPipelineMode::Sequential);
    }
    
    // Display zero-copy mode
    std::cout << "Zero-copy mode: " << (useZeroCopy ? "enabled" : "disabled") << std::endl;
    
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
                if (useZeroCopy) {
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