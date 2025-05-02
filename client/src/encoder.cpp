#include "encoder.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>

Encoder::Encoder() : m_frameCount(0), m_encoder(nullptr), m_firstFrame(true) {
    std::cout << "Encoder created" << std::endl;
    
    // Initialize x264 picture structures
    x264_picture_init(&m_picIn);
    x264_picture_init(&m_picOut);
    
    // Initialize performance counter for timestamp generation
    QueryPerformanceFrequency(&m_frequency);
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
    
    std::cout << "Encoder destroyed" << std::endl;
}

bool Encoder::Initialize(int width, int height, int fps, int bitrate) {
    // Store parameters
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_bitrate = bitrate;
    
    // Clean up any existing encoder
    if (m_encoder) {
        x264_encoder_close(m_encoder);
        m_encoder = nullptr;
    }
    
    // Set up x264 parameters
    x264_param_t param;
    
    // Use slower but more consistent preset for better quality
    x264_param_default_preset(&param, "medium", "zerolatency");
    
    // Configure encoding parameters
    param.i_width = width;
    param.i_height = height;
    param.i_csp = X264_CSP_I420;
    
    // Set framerate
    param.i_fps_num = fps;
    param.i_fps_den = 1;
    
    // Set keyframe interval
    param.i_keyint_max = fps * 2;  // Keyframe every 2 seconds
    
    // Use constant quality mode (CRF) for more consistent quality
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = 23;  // Reasonable CRF value (lower = better quality)
    param.rc.f_rf_constant_max = 28;  // Max CRF for rate control
    
    // Set a maximum bitrate to prevent excessive size
    param.rc.i_vbv_max_bitrate = bitrate / 1000;  // kbps
    param.rc.i_vbv_buffer_size = bitrate / 1000;  // kbps (1 second buffer)
    
    // Set output timebase to ensure consistent timing
    param.i_timebase_num = 1;
    param.i_timebase_den = 90000;  // Standard timebase for H.264
    
    // Use multiple threads for better performance
    param.i_threads = 0;  // Auto-detect threads (usually 1.5x logical cores)
    
    // Configure low-latency settings
    param.b_repeat_headers = 1;  // Repeat SPS/PPS headers for each keyframe
    param.b_annexb = 1;          // Use Annex-B format for H.264 stream
    
    // Disable b-frames for lower latency
    param.i_bframe = 0;
    
    // Enable frame skipping only when really necessary
    param.rc.b_filler = 0;
    param.rc.i_lookahead = fps / 2;  // Half a second of lookahead for better rate control
    
    // Configure profile (Constrained Baseline for maximum compatibility)
    x264_param_apply_profile(&param, "baseline");
    
    // Create encoder
    m_encoder = x264_encoder_open(&param);
    if (!m_encoder) {
        std::cerr << "Failed to create x264 encoder" << std::endl;
        return false;
    }
    
    // Allocate YUV buffer
    m_yuvBuffer.resize(width * height * 3 / 2);
    
    // Set up input picture
    m_picIn.img.i_csp = X264_CSP_I420;
    m_picIn.img.i_plane = 3;
    m_picIn.img.plane[0] = m_yuvBuffer.data();  // Y plane
    m_picIn.img.plane[1] = m_picIn.img.plane[0] + width * height;  // U plane
    m_picIn.img.plane[2] = m_picIn.img.plane[1] + width * height / 4;  // V plane
    m_picIn.img.i_stride[0] = width;
    m_picIn.img.i_stride[1] = width / 2;
    m_picIn.img.i_stride[2] = width / 2;
    
    // Open output file with timestamp
    if (!OpenOutputFile()) {
        std::cerr << "Failed to open output file" << std::endl;
        return false;
    }
    
    // Start the encoding thread
    StartEncodingThread();
    
    // Output encoder settings
    std::cout << "Encoder initialized with " << width << "x" << height 
              << " at " << fps << " fps and CRF quality" << std::endl;
    
    return true;
}

bool Encoder::EncodeFrame(const std::vector<uint8_t>& bgraFrame, int width, int height) {
    // Check if dimensions match
    if (width != m_width || height != m_height) {
        std::cerr << "Frame dimensions don't match encoder settings" << std::endl;
        return false;
    }
    
    // Get current timestamp
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);
    
    // Create a new frame for the queue
    EncoderFrame frame;
    frame.data = bgraFrame;  // Make a copy of the frame data
    frame.width = width;
    frame.height = height;
    frame.timestamp = timestamp.QuadPart;
    
    // Add frame to queue (thread-safe)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        
        // Manage queue size based on queue depth thresholds
        const size_t MAX_QUEUE_SIZE = 15;  // Max 0.5 seconds at 30fps
        const size_t TARGET_QUEUE_SIZE = 5;  // Target ~5 frames in queue for smoothness
        
        if (m_frameQueue.size() > MAX_QUEUE_SIZE) {
            // If queue gets too large, drop multiple frames to catch up quickly
            int framesToDrop = m_frameQueue.size() - TARGET_QUEUE_SIZE;
            
            std::cout << "Warning: Queue too full (" << m_frameQueue.size() 
                      << " frames), dropping " << framesToDrop << " frames" << std::endl;
            
            // Keep the most recent frames to maintain temporal continuity
            while (framesToDrop > 0 && !m_frameQueue.empty()) {
                m_frameQueue.pop();
                framesToDrop--;
            }
        }
        
        m_frameQueue.push(std::move(frame));
    }
    
    // Notify the encoding thread that a new frame is available
    m_queueCV.notify_one();
    
    return true;
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
    // Calculate target frame interval in QPC ticks
    LONGLONG targetInterval = m_frequency.QuadPart / m_fps;
    
    // Keep track of the ideal next frame time based on a stable clock,
    // not based on the previous frame's actual time
    LARGE_INTEGER startTime;
    QueryPerformanceCounter(&startTime);
    LONGLONG nextFrameTime = startTime.QuadPart;
    
    // Stats for monitoring
    int framesEncoded = 0;
    LONGLONG lastStatsTime = startTime.QuadPart;
    
    // Main encoding loop
    while (m_running) {
        EncoderFrame frame;
        bool hasFrame = false;
        
        // Get frame from queue with timeout
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            
            // Calculate how long to wait for a frame
            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            LONGLONG waitTime = nextFrameTime - currentTime.QuadPart;
            
            // Convert to milliseconds
            std::chrono::milliseconds waitMs(1); // Default minimum wait
            if (waitTime > 0) {
                waitMs = std::chrono::milliseconds((waitTime * 1000) / m_frequency.QuadPart);
            }
            
            // Wait for frame or timeout
            auto waitResult = m_queueCV.wait_for(lock, waitMs, 
                [this] { return !m_frameQueue.empty() || !m_running; });
            
            // If we have a frame and we're still running, process it
            if (!m_frameQueue.empty() && m_running) {
                frame = std::move(m_frameQueue.front());
                m_frameQueue.pop();
                hasFrame = true;
            }
        }
        
        // Get current time
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        
        // If current time has passed the next frame time, it's time to encode
        if (currentTime.QuadPart >= nextFrameTime) {
            // If we have a frame, encode it
            if (hasFrame) {
                // Encode the frame
                EncodeFrameInternal(frame);
                framesEncoded++;
                
                // Output stats every second
                if (currentTime.QuadPart - lastStatsTime > m_frequency.QuadPart) {
                    double elapsed = static_cast<double>(currentTime.QuadPart - lastStatsTime) / m_frequency.QuadPart;
                    double fps = framesEncoded / elapsed;
                    std::cout << "Encoder actual FPS: " << fps << std::endl;
                    
                    framesEncoded = 0;
                    lastStatsTime = currentTime.QuadPart;
                }
            }
            
            // Calculate next frame time (based on the ideal timing, not actual)
            nextFrameTime += targetInterval;
            
            // If we've fallen too far behind, reset timing
            if (currentTime.QuadPart > nextFrameTime + targetInterval * 5) {
                std::cout << "Encoder too far behind, resetting timing" << std::endl;
                nextFrameTime = currentTime.QuadPart + targetInterval;
            }
        } else {
            // We're ahead of schedule, sleep until next frame time
            LONGLONG sleepTime = nextFrameTime - currentTime.QuadPart;
            if (sleepTime > 0) {
                DWORD sleepMs = static_cast<DWORD>((sleepTime * 1000) / m_frequency.QuadPart);
                if (sleepMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                }
            }
        }
    }
}

bool Encoder::EncodeFrameInternal(const EncoderFrame& frame) {
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
    // Safety check for input buffer size
    if (bgraFrame.size() < m_width * m_height * 4) {
        std::cerr << "Input buffer too small for BGRA data" << std::endl;
        return false;
    }
    
    const uint8_t* bgra = bgraFrame.data();
    uint8_t* y = m_picIn.img.plane[0];
    uint8_t* u = m_picIn.img.plane[1];
    uint8_t* v = m_picIn.img.plane[2];
    
    // Clear U and V planes
    std::memset(u, 128, m_width * m_height / 4);
    std::memset(v, 128, m_width * m_height / 4);
    
    // Convert BGRA to YUV420
    for (int i = 0; i < m_height; i++) {
        for (int j = 0; j < m_width; j++) {
            int index = (i * m_width + j) * 4;
            
            // BGRA to YUV
            int b = bgra[index + 0];
            int g = bgra[index + 1];
            int r = bgra[index + 2];
            
            // Y plane (all pixels)
            y[i * m_width + j] = static_cast<uint8_t>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            
            // U and V planes (subsampled)
            if (i % 2 == 0 && j % 2 == 0) {
                int uIndex = (i / 2) * (m_width / 2) + (j / 2);
                int r_avg = r;
                int g_avg = g;
                int b_avg = b;
                
                // Very simple averaging for 2x2 block
                if (j + 1 < m_width && i + 1 < m_height) {
                    r_avg = (r + bgra[index + 4 + 2] + bgra[index + m_width * 4 + 2] + bgra[index + m_width * 4 + 4 + 2]) / 4;
                    g_avg = (g + bgra[index + 4 + 1] + bgra[index + m_width * 4 + 1] + bgra[index + m_width * 4 + 4 + 1]) / 4;
                    b_avg = (b + bgra[index + 4 + 0] + bgra[index + m_width * 4 + 0] + bgra[index + m_width * 4 + 4 + 0]) / 4;
                }
                
                u[uIndex] = static_cast<uint8_t>(((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128);
                v[uIndex] = static_cast<uint8_t>(((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128);
            }
        }
    }
    
    return true;
}

bool Encoder::OpenOutputFile() {
    // Create a filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    
    char buffer[100];
    std::strftime(buffer, sizeof(buffer), "capture_%Y%m%d_%H%M%S", &tm);
    
    std::string filename = std::string(buffer) + "_" + std::to_string(m_width) + "x" + std::to_string(m_height) + ".h264";
    
    m_outputFile.open(filename, std::ios::binary);
    if (!m_outputFile.is_open()) {
        std::cerr << "Failed to open output file: " << filename << std::endl;
        return false;
    }
    
    // Output file headers for raw H.264
    OutputHeaders();
    
    std::cout << "Recording to file: " << filename << std::endl;
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