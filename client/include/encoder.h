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

class Encoder {
public:
    Encoder();
    ~Encoder();

    bool Initialize(int width, int height, int fps = 30, int bitrate = 2000000);
    bool EncodeFrame(const std::vector<uint8_t>& bgraFrame, int width, int height);
    
    // Set callback for encoded data
    void SetCallback(EncodedFrameCallback callback) { m_callback = callback; }

private:
    // File output methods
    bool OpenOutputFile();
    void CloseOutputFile();
    void OutputHeaders();
    
    // Threaded encoding
    void EncodingThreadFunc();
    void StartEncodingThread();
    void StopEncodingThread();
    
    // Basic encoder state
    int m_width = 0;
    int m_height = 0;
    int m_frameCount = 0;
    int m_fps = 30;
    int m_bitrate = 2000000;
    
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
    
    // Output file
    std::ofstream m_outputFile;
    
    // x264 specific members
    x264_t* m_encoder = nullptr;
    x264_picture_t m_picIn;
    x264_picture_t m_picOut;
    
    // YUV buffer for color conversion
    std::vector<uint8_t> m_yuvBuffer;
    
    // Helper methods
    bool ConvertBGRAtoYUV(const std::vector<uint8_t>& bgraFrame);
    bool EncodeFrameInternal(const EncoderFrame& frame);
}; 