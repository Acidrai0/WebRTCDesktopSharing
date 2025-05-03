#include "encoder.h"
#include "yuv_converter.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>
#include <windows.h>

Encoder::Encoder() : m_frameCount(0), m_encoder(nullptr), m_firstFrame(true), m_actualFps(0.0f), m_statsFrameCount(0), m_statsInterval(30), m_statsStartTime(0), m_encodedBytes(0), m_fps(30), m_testName("") {
    std::cout << "Encoder created" << std::endl;
    
    // Initialize YUV converter
    m_yuvConverter = std::make_unique<YuvConverter>();
    
    // Initialize x264 picture structures
    x264_picture_init(&m_picIn);
    x264_picture_init(&m_picOut);
    
    // Initialize performance counter for timestamp generation
    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_lastStatsTime);
    
    // Get current time for stats
    m_statsStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Encoder::~Encoder() {
    // Stop the encoding thread
    StopEncodingThread();
    
    // Close output file if open
    CloseOutputFile();
    
    // Clean up x264 resources
    if (m_encoder) {
        x264_encoder_close(m_encoder);
        m_encoder = nullptr;
    }
    
    // Clean up picture resources
    x264_picture_clean(&m_picIn);
    
    // YuvConverter will clean up automatically
    
    std::cout << "Encoder destroyed" << std::endl;
}

bool Encoder::Initialize(int width, int height, int fps, int bitrate, const std::string& testName) {
    m_width = width;
    m_height = height;
    m_bitrate = bitrate;
    m_fps = fps;
    m_testName = testName;
    
    // Configure x264 parameters
    x264_param_t param;
    x264_param_default_preset(&param, "veryfast", "zerolatency");
    
    // Set the resolution and color space
    param.i_width = width;
    param.i_height = height;
    param.i_csp = X264_CSP_I420;
    
    // Set explicit color space parameters (fixes blue tint)
    param.vui.i_colorprim = 1;     // BT.709
    param.vui.i_transfer = 1;      // BT.709
    param.vui.i_colmatrix = 1;     // BT.709
    param.vui.b_fullrange = 0;     // Limited range (16-235)
    
    // Set rate control
    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = bitrate;
    param.rc.i_vbv_max_bitrate = bitrate * 1.5;
    param.rc.i_vbv_buffer_size = bitrate;
    
    // Set performance options
    param.i_threads = 4;                    // Auto-detect thread count
    param.b_sliced_threads = 1;             // Sliced threading for lower latency
    param.i_lookahead_threads = 0;          // No lookahead threads (minimizes latency)
    
    // Set GOP (keyframe interval) options
    param.i_keyint_max = 60;                // Keyframe every 2 seconds at 30fps
    param.i_keyint_min = 10;                // Minimum keyframe interval
    
    // Set output format
    param.b_repeat_headers = 1;             // Repeat SPS/PPS headers
    param.b_annexb = 1;                     // Use Annex-B format for bitstream
    
    // Frame rate - we'll calculate this dynamically
    param.i_fps_num = 30;
    param.i_fps_den = 1;
    
    // Open encoder
    m_encoder = x264_encoder_open(&param);
    if (!m_encoder) {
        std::cerr << "Failed to open x264 encoder" << std::endl;
        return false;
    }
    
    // Allocate x264 picture
    m_picIn.img.i_csp = X264_CSP_I420;
    m_picIn.img.i_plane = 3;
    
    std::cout << "Encoder initialized: " << width << "x" << height 
              << ", bitrate: " << bitrate << " kbps" << std::endl;
    
    // Always open output file
    if (OpenOutputFile()) {
        if (!m_testName.empty()) {
            std::cout << "File recording started with test name: " << m_testName << std::endl;
        } else {
            std::cout << "File recording started" << std::endl;
        }
    } else {
        std::cerr << "Failed to open output file for recording" << std::endl;
    }
    
    // Start the encoding thread
    StartEncodingThread();
    
    return true;
}

bool Encoder::EncodeFrame(const std::vector<uint8_t>& bgraFrame, int width, int height) {
    if (!m_encoder) {
        std::cerr << "Encoder not initialized" << std::endl;
        return false;
    }
    
    // Create a frame object and add it to the queue
    EncoderFrame frame;
    frame.data = bgraFrame;
    frame.width = width;
    frame.height = height;
    frame.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    // Add frame to encoding queue
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_frameQueue.push(frame);
    }
    
    // Notify encoding thread
    m_queueCV.notify_one();
    
    return true;
}

bool Encoder::EncodeFrameDirect(const std::vector<uint8_t>& bgraFrame,
                         std::vector<uint8_t>& outputData,
                         bool& isKeyFrame) {
    if (!m_encoder) {
        std::cerr << "Encoder not initialized" << std::endl;
        return false;
    }
    
    // Reset output parameters
    outputData.clear();
    isKeyFrame = false;
    
    // Convert BGRA to YUV using libyuv
    if (!ConvertBGRAtoYUV(bgraFrame)) {
        std::cerr << "Failed to convert frame to YUV" << std::endl;
        return false;
    }
    
    // Set pts
    m_picIn.i_pts = m_frameCount++;
    
    // Encode the frame
    x264_nal_t* nals;
    int numNals;
    int frameSize;
    
    frameSize = x264_encoder_encode(m_encoder, &nals, &numNals, &m_picIn, &m_picOut);
    
    if (frameSize < 0) {
        std::cerr << "x264 encoding failed: " << frameSize << std::endl;
        return false;
    }
    
    // Skip empty frames
    if (frameSize == 0) {
        return true; // No output generated but not an error
    }
    
    // Check if this is a keyframe
    isKeyFrame = m_picOut.b_keyframe != 0;
    
    // Copy encoded data to output buffer
    outputData.resize(frameSize);
    memcpy(outputData.data(), nals[0].p_payload, frameSize);
    
    // Track encoding stats
    m_encodedBytes += frameSize;
    if (m_firstFrame) {
        m_firstPts = m_picOut.i_pts;
        m_firstFrame = false;
    }
    m_lastPts = m_picOut.i_pts;
    
    // Update statistics if needed
    ProcessStats(nals, numNals, &m_picOut);
    
    return true;
}

bool Encoder::EncodeFrameInternal(const EncoderFrame& frame) {
    // Debug output for frame size verification
    size_t expectedSize = m_width * m_height * 4; // BGRA is 4 bytes per pixel
    if (frame.data.size() < expectedSize) {
        std::cerr << "EncodeFrameInternal: Frame buffer too small. "
                  << "Expected: " << expectedSize << " bytes, "
                  << "Got: " << frame.data.size() << " bytes" << std::endl;
    }
    
    // Convert BGRA to YUV420P (I420)
    if (!ConvertBGRAtoYUV(frame.data)) {
        std::cerr << "Failed to convert BGRA to YUV" << std::endl;
        return false;
    }
    
    // Set timestamp in presentation order
    m_picIn.i_pts = m_frameCount;
    
    // Set dts to be the same as pts for consistent timing
    m_picIn.i_dts = m_picIn.i_pts;
    
    // Increment frame counter
    m_frameCount++;
    
    // Encode frame
    x264_nal_t* nals;
    int i_nals;
    int frameSize = x264_encoder_encode(m_encoder, &nals, &i_nals, &m_picIn, &m_picOut);
    
    if (frameSize < 0) {
        std::cerr << "Failed to encode frame" << std::endl;
        return false;
    }
    
    // No output yet
    if (frameSize == 0) {
        return true;
    }
    
    // Frame encoded successfully - write to file and call callback
    if (m_outputFile.is_open()) {
        m_outputFile.write(reinterpret_cast<char*>(nals[0].p_payload), frameSize);
    }
    
    // Call callback if registered
    if (m_callback) {
        bool isKeyFrame = m_picOut.b_keyframe != 0;
        m_callback(nals[0].p_payload, frameSize, isKeyFrame);
    }
    
    // Print encoding stats (not for every frame to reduce console spam)
    static int logCounter = 0;
    if (m_picOut.b_keyframe || ++logCounter % 10 == 0) { // Log keyframes and every 10th frame
        std::cout << "Encoding frame " << (m_frameCount - 1) << " (" << frame.width << "x" << frame.height 
                << ") size: " << frameSize << " bytes";
        if (m_picOut.b_keyframe) {
            std::cout << " [KEYFRAME]";
        }
        std::cout << " PTS: " << m_picOut.i_pts << std::endl;
    }
    
    return true;
}

bool Encoder::ConvertBGRAtoYUV(const std::vector<uint8_t>& bgraFrame) {
    if (bgraFrame.size() < m_width * m_height * 4) {
        std::cerr << "Input frame too small for resolution: " 
                  << bgraFrame.size() << " vs " << (m_width * m_height * 4) << std::endl;
        return false;
    }
    
    // Convert BGRA to I420 using libyuv
    uint8_t* yPlane = nullptr;
    uint8_t* uPlane = nullptr;
    uint8_t* vPlane = nullptr;
    int yStride, uStride, vStride;
    
    bool success = m_yuvConverter->ConvertBGRAtoI420(
        bgraFrame.data(),          // BGRA data
        m_width * 4,               // BGRA stride
        m_width, m_height,         // Dimensions
        &yPlane, &uPlane, &vPlane, // Output planes
        &yStride, &uStride, &vStride // Output strides
    );
    
    if (!success) {
        std::cerr << "YUV conversion failed" << std::endl;
        return false;
    }
    
    // Set the x264 picture planes and strides
    m_picIn.img.i_stride[0] = yStride;
    m_picIn.img.i_stride[1] = uStride;
    m_picIn.img.i_stride[2] = vStride;
    m_picIn.img.plane[0] = yPlane;
    m_picIn.img.plane[1] = uPlane;
    m_picIn.img.plane[2] = vPlane;
    
    return true;
}

void Encoder::ProcessStats(x264_nal_t* nals, int numNals, x264_picture_t* pic_out) {
    // Update stats every N frames
    if (++m_statsFrameCount >= m_statsInterval) {
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        
        // Calculate elapsed time since last stats update
        double elapsedSeconds = (double)(currentTime.QuadPart - m_lastStatsTime.QuadPart) / m_frequency.QuadPart;
        
        // Calculate actual fps
        m_actualFps = m_statsFrameCount / elapsedSeconds;
        
        // Calculate bitrate (bits per second)
        float bitrate = (float)(m_encodedBytes * 8) / 1000.0f / elapsedSeconds;
        
        // Get current queue size
        int queueSize = m_frameQueue.size();
        
        // Log statistics
        std::cout << "Encoder stats: " << m_actualFps << " fps, " 
                  << bitrate << " kbps, Queue: " << queueSize << std::endl;
        
        // Call stats callback if registered
        if (m_statsCallback) {
            m_statsCallback(m_actualFps, queueSize);
        }
        
        // Reset statistics
        m_statsFrameCount = 0;
        m_encodedBytes = 0;
        m_lastStatsTime = currentTime;
    }
}

void Encoder::StartEncodingThread() {
    // Set the running flag
    m_running = true;
    
    // Start the encoding thread
    m_encodingThread = std::thread(&Encoder::EncodingThreadFunc, this);
}

void Encoder::StopEncodingThread() {
    // Stop the thread
    if (m_running) {
        m_running = false;
        m_queueCV.notify_all();  // Wake up the thread if it's waiting
        
        // Wait for the thread to finish
        if (m_encodingThread.joinable()) {
            m_encodingThread.join();
        }
    }
}

void Encoder::EncodingThreadFunc() {
    std::cout << "Encoding thread started" << std::endl;
    
    // Initialize stats tracking
    QueryPerformanceCounter(&m_lastStatsTime);
    
    // Stats for thread
    int framesEncoded = 0;
    
    while (m_running) {
        EncoderFrame frame;
        bool hasFrame = false;
        
        // Get a frame from the queue
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            
            // Wait for a frame or shutdown signal
            m_queueCV.wait(lock, [this] {
                return !m_frameQueue.empty() || !m_running;
            });
            
            // Check if we should exit
            if (!m_running && m_frameQueue.empty()) {
                break;
            }
            
            // Get next frame
            if (!m_frameQueue.empty()) {
                frame = m_frameQueue.front();
                m_frameQueue.pop();
                hasFrame = true;
            }
        }
        
        // Process the frame if we have one
        if (hasFrame) {
            // Process the frame using the internal encoding method
            if (EncodeFrameInternal(frame)) {
                framesEncoded++;
                m_statsFrameCount++;
            }
            
            // Update stats periodically
            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            double elapsedSeconds = (double)(currentTime.QuadPart - m_lastStatsTime.QuadPart) / m_frequency.QuadPart;
            
            if (elapsedSeconds >= 1.0) {  // Every second
                // Calculate FPS and queue size
                m_actualFps = framesEncoded / elapsedSeconds;
                int queueSize = m_frameQueue.size();
                
                // Call stats callback if registered
                if (m_statsCallback) {
                    m_statsCallback(m_actualFps, queueSize);
                }
                
                // Log to console
                std::cout << "Thread stats: " << m_actualFps << " fps, Queue: " << queueSize << std::endl;
                
                // Reset stats
                framesEncoded = 0;
                m_lastStatsTime = currentTime;
            }
        }
    }
    
    std::cout << "Encoding thread stopped" << std::endl;
}

bool Encoder::OpenOutputFile() {
    // Create a filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    
    char buffer[100];
    std::strftime(buffer, sizeof(buffer), "capture_%Y%m%d_%H%M%S", &tm);
    
    std::string filename;
    if (!m_testName.empty()) {
        filename = std::string(buffer) + "_" + m_testName + "_" + std::to_string(m_width) + "x" + std::to_string(m_height) + ".h264";
    } else {
        filename = std::string(buffer) + "_" + std::to_string(m_width) + "x" + std::to_string(m_height) + ".h264";
    }
    
    // Get executable path and save file there
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    
    // Extract directory from the executable path
    std::string exePathStr(exePath);
    size_t lastSlash = exePathStr.find_last_of("\\/");
    std::string exeDir = exePathStr.substr(0, lastSlash + 1);
    
    // Create full path
    std::string fullPath = exeDir + filename;
    
    m_outputFile.open(fullPath, std::ios::binary);
    if (!m_outputFile.is_open()) {
        std::cerr << "Failed to open output file: " << fullPath << std::endl;
        return false;
    }
    
    // Output file headers for raw H.264
    OutputHeaders();
    
    std::cout << "Recording to file: " << fullPath << std::endl;
    return true;
}

void Encoder::CloseOutputFile() {
    if (m_outputFile.is_open()) {
        // Flush any delayed frames
        if (m_encoder) {
            x264_nal_t* nals;
            int i_nals;
            while (x264_encoder_delayed_frames(m_encoder)) {
                int frameSize = x264_encoder_encode(m_encoder, &nals, &i_nals, nullptr, &m_picOut);
                if (frameSize > 0) {
                    m_outputFile.write(reinterpret_cast<char*>(nals[0].p_payload), frameSize);
                }
            }
        }
        
        m_outputFile.close();
        std::cout << "Recording stopped" << std::endl;
    }
}

void Encoder::OutputHeaders() {
    if (!m_encoder || !m_outputFile.is_open()) {
        return;
    }
    
    x264_nal_t* nals;
    int i_nals;
    
    // Write SPS/PPS headers
    int headerSize = x264_encoder_headers(m_encoder, &nals, &i_nals);
    if (headerSize > 0) {
        for (int i = 0; i < i_nals; i++) {
            m_outputFile.write(reinterpret_cast<char*>(nals[i].p_payload), nals[i].i_payload);
        }
    }
}

float Encoder::GetFPS() const {
    return m_actualFps;
} 