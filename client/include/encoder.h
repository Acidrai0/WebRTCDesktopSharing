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
#include <deque>

// Forward declarations
class YuvConverter;

// x264 includes
extern "C" {
#include <x264.h>
}

// Frame structure to hold capture data for encoding
struct EncoderFrame {
    // Frame data - will be null for zero-copy mode
    std::vector<uint8_t> data;
    
    // Use a shared_ptr for zero-copy across threads
    std::shared_ptr<std::vector<uint8_t>> sharedData;
    
    int width = 0;
    int height = 0;
    int64_t timestamp = 0;  // Timestamp in QPC ticks
    int64_t qpcTimestamp = 0; // Raw QPC timestamp for high-precision timing
    
    // YUV data
    uint8_t* yPlane = nullptr;
    uint8_t* uPlane = nullptr;
    uint8_t* vPlane = nullptr;
    int yStride = 0;
    int uStride = 0;
    int vStride = 0;
    bool yuvConverted = false;
    
    // Track if this frame is from the pool
    bool fromPool = false;
    int poolIndex = -1;
    
    void Reset() {
        // Reset the frame but don't deallocate memory
        if (!fromPool) {
            data.clear();
            sharedData.reset();
        }
        width = 0;
        height = 0;
        timestamp = 0;
        qpcTimestamp = 0;
        yPlane = nullptr;
        uPlane = nullptr;
        vPlane = nullptr;
        yStride = 0;
        uStride = 0;
        vStride = 0;
        yuvConverted = false;
    }
};

// Frame pool for zero-allocation encoding
class FramePool {
public:
    FramePool(size_t initialCapacity = 8, size_t initialBufferSize = 4 * 1920 * 1080) 
        : m_initialBufferSize(initialBufferSize) {
        // Pre-allocate frames
        for (size_t i = 0; i < initialCapacity; i++) {
            EncoderFrame frame;
            frame.data.reserve(initialBufferSize);
            frame.fromPool = true;
            frame.poolIndex = static_cast<int>(i);
            m_availableFrames.push_back(frame);
        }
    }
    
    EncoderFrame* GetFrame() {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        
        if (m_availableFrames.empty()) {
            // Create a new frame if none available
            EncoderFrame frame;
            frame.data.reserve(m_initialBufferSize);
            frame.fromPool = true;
            frame.poolIndex = static_cast<int>(m_frames.size());
            m_frames.push_back(frame);
            return &m_frames.back();
        }
        
        // Get a frame from available frames
        m_frames.push_back(m_availableFrames.front());
        m_availableFrames.pop_front();
        
        return &m_frames.back();
    }
    
    void ReleaseFrame(EncoderFrame* frame) {
        if (!frame || !frame->fromPool) return;
        
        std::lock_guard<std::mutex> lock(m_poolMutex);
        
        // Find the frame in the active frames
        for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
            if (it->poolIndex == frame->poolIndex) {
                // Reset frame state but keep buffer allocated
                it->Reset();
                // Move to available frames
                m_availableFrames.push_back(*it);
                // Remove from active frames
                m_frames.erase(it);
                break;
            }
        }
    }
    
private:
    std::deque<EncoderFrame> m_availableFrames;
    std::deque<EncoderFrame> m_frames;
    std::mutex m_poolMutex;
    size_t m_initialBufferSize;
};

// Define a callback type for receiving encoded H.264 data
using EncodedFrameCallback = std::function<void(const uint8_t* data, size_t size, bool isKeyFrame)>;

// Define a callback type for receiving encoder statistics
using EncoderStatsCallback = std::function<void(float fps, int queueSize)>;

// Pipeline modes for the encoder
enum class EncoderPipelineMode {
    Sequential,    // Original single-threaded pipeline
    Parallel       // Parallel pipeline with separate conversion and encoding threads
};

class Encoder {
public:
    Encoder();
    ~Encoder();

    // Initialize the encoder
    bool Initialize(int width, int height, int fps = 30, int bitrate = 5000, const std::string& testName = "");
    
    // Configure the pipeline mode
    void SetPipelineMode(EncoderPipelineMode mode);
    
    // New method for queueing frames for threaded encoding
    bool EncodeFrame(const std::vector<uint8_t>& bgraFrame, int width, int height);
    
    // Zero-copy version that uses a shared pointer
    bool EncodeFrameZeroCopy(const std::shared_ptr<std::vector<uint8_t>>& bgraFrame, int width, int height);
    
    // Set callback for encoded data
    void SetCallback(EncodedFrameCallback callback) { m_callback = callback; }
    
    // Set callback for encoder statistics
    void SetStatsCallback(EncoderStatsCallback callback) { m_statsCallback = callback; }

    // Get current encoding FPS
    float GetFPS() const;
    
    // Get pipeline statistics
    struct PipelineStats {
        float captureRate = 0.0f;
        float conversionRate = 0.0f;
        float encodingRate = 0.0f;
        int rawQueueSize = 0;
        int yuvQueueSize = 0;
        float endToEndLatencyMs = 0.0f;
    };
    
    PipelineStats GetPipelineStats() const;

private:
    // File output methods
    bool OpenOutputFile();
    void CloseOutputFile();
    void OutputHeaders();
    
    // Statistics processing method
    void ProcessStats(x264_nal_t* nals, int numNals, x264_picture_t* pic_out);
    
    // Threaded pipeline stages
    void StartPipeline();
    void StopPipeline();
    
    // Thread functions for each pipeline stage
    void PreprocessThreadFunc();   // YUV conversion
    void EncodingThreadFunc();     // H.264 encoding
    
    // Method for encoding a frame (used by the encoding thread)
    bool EncodeFrameInternal(const EncoderFrame& frame);
    
    // Direct frame encoding method (non-threaded)
    bool EncodeFrameDirect(const std::vector<uint8_t>& bgraFrame,
                           std::vector<uint8_t>& outputData,
                           bool& isKeyFrame);
    
    // Helper methods
    bool ConvertBGRAtoYUV(const std::vector<uint8_t>& bgraFrame, EncoderFrame& frame);
    bool ConvertBGRAtoYUV(const std::shared_ptr<std::vector<uint8_t>>& bgraFrame, EncoderFrame& frame);
    bool ConvertBGRAtoYUV(const std::vector<uint8_t>& bgraFrame); // Original method for backward compatibility
    
    // Frame pool for zero-allocation encoding (aligned to cache line boundary)
    alignas(64) FramePool m_framePool;
    
    // Basic encoder state
    int m_width = 0;
    int m_height = 0;
    int m_frameCount = 0;
    int m_bitrate = 5000;
    int m_fps = 30;              // Target frames per second
    
    // Windows-specific frame timing using QPC
    LARGE_INTEGER m_frequency;       // Performance counter frequency
    LARGE_INTEGER m_lastCaptureTime; // Last capture timestamp
    bool m_firstFrame = true;        // First frame flag
    
    // Pipeline mode
    EncoderPipelineMode m_pipelineMode = EncoderPipelineMode::Sequential;
    
    // Thread synchronization for parallel pipeline
    std::thread m_preprocessThread;
    std::thread m_encodingThread;
    std::atomic<bool> m_running{false};
    
    // Frame queues for pipeline stages
    std::queue<EncoderFrame*> m_rawFrameQueue;    // Raw BGRA frames (pointers for zero-copy)
    std::queue<EncoderFrame*> m_yuvFrameQueue;    // YUV-converted frames (pointers for zero-copy)
    
    // Locks for pipeline queues
    std::mutex m_rawQueueMutex;
    std::mutex m_yuvQueueMutex;
    std::condition_variable m_rawQueueCV;
    std::condition_variable m_yuvQueueCV;
    
    // Original threading components (for backward compatibility)
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    std::queue<EncoderFrame> m_frameQueue;
    
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
    
    // Pipeline statistics
    struct {
        std::atomic<float> captureRate{0.0f};
        std::atomic<float> conversionRate{0.0f};
        std::atomic<float> encodingRate{0.0f};
        std::atomic<float> endToEndLatencyMs{0.0f};
    } m_pipelineStats;

    // Encoder parameters
    int64_t m_firstPts = 0;          // First frame PTS
    int64_t m_lastPts = 0;           // Last frame PTS
    int64_t m_encodedBytes = 0;      // Total encoded bytes
    int64_t m_statsStartTime = 0;    // Start time for statistics

    // YUV conversion buffer management
    std::unique_ptr<YuvConverter> m_yuvConverter;

    // Timing smoothing variables for consistent frame rate
    std::chrono::time_point<std::chrono::steady_clock> m_lastReleaseTime;
    double m_targetFrameInterval; // in milliseconds (1000/fps)
    bool m_useTimingBuffer = true; // Enable frame rate smoothing
    
    // Frame timing helper functions
    void InitializeTimingBuffer(int fps);
    bool ShouldReleaseFrame(const EncoderFrame* frame);
    void UpdateTimingBuffer();
}; 