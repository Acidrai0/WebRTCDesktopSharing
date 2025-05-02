#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <fstream>
#include <chrono>

// x264 includes
extern "C" {
#include <x264.h>
}

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
    
    // Basic encoder state
    int m_width = 0;
    int m_height = 0;
    int m_frameCount = 0;
    int m_fps = 30;
    int m_bitrate = 2000000;
    
    // Frame timing
    std::chrono::time_point<std::chrono::steady_clock> m_startTime;
    int64_t m_lastPts = 0;
    bool m_firstFrame = true;
    
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
}; 