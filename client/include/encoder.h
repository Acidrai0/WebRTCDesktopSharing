#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <fstream>
#include <chrono>
#include <Windows.h>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <string>

// Forward declarations
class YuvConverter;

// x264 includes
extern "C" {
#include <x264.h>
}

// Frame structure to hold capture data for encoding
struct EncoderFrame {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int64_t timestamp = 0;  // Timestamp in QPC ticks
};

// Define a callback type for receiving encoded H.264 data
using EncodedFrameCallback = std::function<void(const uint8_t* data, size_t size, bool isKeyFrame)>;

// Define a callback type for receiving encoder statistics
using EncoderStatsCallback = std::function<void(float fps, int queueSize)>;

class Encoder {
public:
    Encoder();
    ~Encoder();

    // Initialize the encoder
    bool Initialize(int width, int height, int fps = 30, int bitrate = 5000, const std::string& testName = "");
    
    // New method for queueing frames for threaded encoding
    bool EncodeFrame(const std::vector<uint8_t>& bgraFrame, int width, int height);
    
    // Set callback for encoded data
    void SetCallback(EncodedFrameCallback callback) { m_callback = callback; }
    
    // Set callback for encoder statistics
    void SetStatsCallback(EncoderStatsCallback callback) { m_statsCallback = callback; }

    // Get current encoding FPS
    float GetFPS() const;

private:
    // File output methods
    bool OpenOutputFile();
    void CloseOutputFile();
    void OutputHeaders();
    
    // Statistics processing method
    void ProcessStats(x264_nal_t* nals, int numNals, x264_picture_t* pic_out);
    
    // Threaded encoding
    void EncodingThreadFunc();
    void StartEncodingThread();
    void StopEncodingThread();
    
    // Method for encoding a frame (used by the encoding thread)
    bool EncodeFrameInternal(const EncoderFrame& frame);
    
    // Direct frame encoding method (non-threaded)
    bool EncodeFrameDirect(const std::vector<uint8_t>& bgraFrame,
                           std::vector<uint8_t>& outputData,
                           bool& isKeyFrame);
    
    // Helper methods
    bool ConvertBGRAtoYUV(const std::vector<uint8_t>& bgraFrame);
    
    // Basic encoder state
    int m_width = 0;
    int m_height = 0;
    int m_frameCount = 0;
    int m_bitrate = 5000;
    int m_fps = 30;              // Target frames per second
    
    // Windows-specific frame timing using QPC
    LARGE_INTEGER m_frequency;       // Performance counter frequency
    bool m_firstFrame = true;        // First frame flag
    
    // Thread synchronization
    std::thread m_encodingThread;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    std::queue<EncoderFrame> m_frameQueue;
    std::atomic<bool> m_running{false};
    
    // Callback for encoded data
    EncodedFrameCallback m_callback;
    
    // Callback for statistics
    EncoderStatsCallback m_statsCallback;
    
    // Output file
    std::ofstream m_outputFile;
    std::string m_testName;          // Test name for file naming
    
    // x264 specific members
    x264_t* m_encoder = nullptr;
    x264_picture_t m_picIn;
    x264_picture_t m_picOut;
    
    // YUV buffer for color conversion
    std::vector<uint8_t> m_yuvBuffer;
    
    // Performance statistics
    float m_actualFps = 0.0f;
    LARGE_INTEGER m_lastStatsTime;
    int m_statsFrameCount = 0;
    int m_statsInterval = 30;        // Calculate stats every N frames

    // Encoder parameters
    int64_t m_firstPts = 0;          // First frame PTS
    int64_t m_lastPts = 0;           // Last frame PTS
    int64_t m_encodedBytes = 0;      // Total encoded bytes
    int64_t m_statsStartTime = 0;    // Start time for statistics

    // YUV conversion buffer management
    std::unique_ptr<YuvConverter> m_yuvConverter;
}; 