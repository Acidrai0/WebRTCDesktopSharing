#include "encoder.h"
#include "yuv_converter.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>
#include <windows.h>
#include <emmintrin.h>

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
    // Stop the pipeline threads
    StopPipeline();
    
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

void Encoder::SetPipelineMode(EncoderPipelineMode mode) {
    // Only allow changing mode when not running
    if (!m_running) {
        m_pipelineMode = mode;
        std::cout << "Encoder pipeline mode set to: " 
                  << (mode == EncoderPipelineMode::Parallel ? "Parallel" : "Sequential") 
                  << std::endl;
    } else {
        std::cerr << "Cannot change pipeline mode while encoder is running" << std::endl;
    }
}

void Encoder::SetBufferSystemMode(BufferSystemMode mode) {
    // Only allow changing mode when not running
    if (!m_running) {
        m_bufferMode = mode;
        std::cout << "Encoder buffer system mode set to: " 
                  << (mode == BufferSystemMode::Queue ? "Queue" : "RingBuffer") 
                  << std::endl;
    } else {
        std::cerr << "Cannot change buffer system mode while encoder is running" << std::endl;
    }
}

bool Encoder::Initialize(int width, int height, int fps, int bitrate, const std::string& testName) {
    m_width = width;
    m_height = height;
    m_bitrate = bitrate;
    m_fps = fps;
    m_testName = testName;
    
    // Initialize timing buffer for smooth frame rate
    InitializeTimingBuffer(fps);
    
    // Initialize the clock if using ring buffer
    if (m_bufferMode == BufferSystemMode::RingBuffer) {
        m_clock = GlobalClock(fps);
        
        // Set ring buffer capacities based on FPS
        // Capture buffer: 2 seconds of frames
        int captureCapacity = fps * 2;
        // Encode buffer: 1 second of frames
        int encodeCapacity = fps;
        
        // Initialize ring buffers with appropriate capacities
        m_rawFrameBuffer = std::make_unique<RingBuffer<std::shared_ptr<EncoderFrame>>>(captureCapacity);
        m_yuvFrameBuffer = std::make_unique<RingBuffer<std::shared_ptr<EncoderFrame>>>(encodeCapacity);
        
        std::cout << "Initialized ring buffers with " 
                  << captureCapacity << " frames capture capacity and "
                  << encodeCapacity << " frames encode capacity" << std::endl;
    }
    
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
    
    // Set for fixed frame rate to ensure proper playback speed
    param.i_fps_num = fps;
    param.i_fps_den = 1;
    
    // Use constant frame rate input instead of variable frame rate
    param.b_vfr_input = 0;                  // Use constant frame rate input
    
    // Force timestamps to match frames exactly
    param.i_timebase_num = 1;               // Set timebase to match fps
    param.i_timebase_den = fps;             // 1/fps second timebase
    
    // Allow delayed frames to maintain quality with acceptable latency
    param.i_bframe = 0;                     // Disable B-frames for lower latency
    param.rc.i_lookahead = 0;               // Disable lookahead for lower latency
    param.i_sync_lookahead = 0;             // Disable sync lookahead
    
    // Allow a delay of up to 3 seconds for buffer (acceptable per user requirements)
    param.rc.f_rf_constant = 22;            // Set a constant quality factor (CRF)
    param.rc.i_aq_mode = 1;                 // Enable adaptive quantization for better quality
    
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
    
    // Start the pipeline (either sequential or parallel)
    StartPipeline();
    
    return true;
}

bool Encoder::EncodeFrame(const std::vector<uint8_t>& bgraFrame, int width, int height) {
    if (!m_encoder) {
        std::cerr << "Encoder not initialized" << std::endl;
        return false;
    }
    
    // Get time for latency measurement
    auto now = std::chrono::high_resolution_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    if (m_pipelineMode == EncoderPipelineMode::Parallel) {
        // Get a frame from the pool
        EncoderFrame* frame = m_framePool.GetFrame();
        
        // Resize if needed and copy data
        if (frame->data.size() < bgraFrame.size()) {
            frame->data.resize(bgraFrame.size());
        }
        
        // Copy data into the frame
        std::memcpy(frame->data.data(), bgraFrame.data(), bgraFrame.size());
        frame->width = width;
        frame->height = height;
        frame->timestamp = timestamp;
        frame->yuvConverted = false;
        
        // For parallel mode, add to raw frame queue
        {
            std::lock_guard<std::mutex> lock(m_rawQueueMutex);
            m_rawFrameQueue.push(frame);
        }
        m_rawQueueCV.notify_one();
    } else {
        // For sequential mode, use original queue with value semantics
        EncoderFrame frame;
        frame.data = bgraFrame;
        frame.width = width;
        frame.height = height;
        frame.timestamp = timestamp;
        
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_frameQueue.push(frame);
        }
        m_queueCV.notify_one();
    }
    
    return true;
}

// Add zero-copy version
bool Encoder::EncodeFrameZeroCopy(const std::shared_ptr<std::vector<uint8_t>>& bgraFrame, int width, int height) {
    if (!m_encoder) {
        std::cerr << "Encoder not initialized" << std::endl;
        return false;
    }
    
    // Get time for latency measurement
    LARGE_INTEGER qpcTimestamp;
    QueryPerformanceCounter(&qpcTimestamp);
    auto now = std::chrono::high_resolution_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    if (m_bufferMode == BufferSystemMode::RingBuffer) {
        // Using ring buffer system
        auto frame = std::make_shared<EncoderFrame>();
        frame->sharedData = bgraFrame;
        frame->width = width;
        frame->height = height;
        frame->timestamp = timestamp;
        frame->qpcTimestamp = qpcTimestamp.QuadPart;
        
        // Push to ring buffer with non-blocking mode to avoid stalling the capture thread
        if (!m_rawFrameBuffer->push(frame, false)) {
            // Buffer full - frame dropped
            std::cout << "WARNING: Capture buffer full, frame dropped" << std::endl;
        }
        
        return true;
    }
    else if (m_pipelineMode == EncoderPipelineMode::Parallel) {
        // Get a frame from the pool
        EncoderFrame* frame = m_framePool.GetFrame();
        
        // Use shared pointer instead of copying
        frame->sharedData = bgraFrame;
        frame->data.clear();  // Don't need this when using shared data
        frame->width = width;
        frame->height = height;
        frame->timestamp = timestamp;
        frame->yuvConverted = false;
        
        // For accurate timing using QPC
        frame->qpcTimestamp = qpcTimestamp.QuadPart;
        
        // For parallel mode, add to raw frame queue
        {
            std::lock_guard<std::mutex> lock(m_rawQueueMutex);
            
            // Calculate and update capture rate
            if (m_rawFrameQueue.empty() && !m_firstFrame) {
                LARGE_INTEGER currentTime;
                QueryPerformanceCounter(&currentTime);
                double elapsed = static_cast<double>(currentTime.QuadPart - m_lastCaptureTime.QuadPart) / 
                                static_cast<double>(m_frequency.QuadPart);
                if (elapsed > 0) {
                    m_pipelineStats.captureRate = 1.0f / static_cast<float>(elapsed);
                }
                m_lastCaptureTime = currentTime;
            } else if (m_firstFrame) {
                QueryPerformanceCounter(&m_lastCaptureTime);
                m_firstFrame = false;
            }
            
            m_rawFrameQueue.push(frame);
            
            // Update stats for monitoring queue growth
            if (m_statsCallback && m_rawFrameQueue.size() > 10) { 
                // Alert if queue getting large
                std::cout << "Warning: Raw frame queue size: " << m_rawFrameQueue.size() << std::endl;
            }
        }
        
        // Notify conversion thread with reduced contention
        m_rawQueueCV.notify_one();
    } else {
        // For sequential mode, use original queue
        EncoderFrame frame;
        frame.sharedData = bgraFrame;
        frame.width = width;
        frame.height = height;
        frame.timestamp = timestamp;
        
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_frameQueue.push(frame);
        }
        m_queueCV.notify_one();
    }
    
    return true;
}

// Start the appropriate pipeline based on mode
void Encoder::StartPipeline() {
    if (m_running) {
        return; // Already running
    }
    
    m_running = true;
    
    if (m_pipelineMode == EncoderPipelineMode::Parallel) {
        std::cout << "Starting parallel encoding pipeline..." << std::endl;
        
        // Start preprocessing thread
        m_preprocessThread = std::thread(&Encoder::PreprocessThreadFunc, this);
        
        // Start encoding thread
        m_encodingThread = std::thread(&Encoder::EncodingThreadFunc, this);
        
        // Set thread priorities if possible
        if (m_preprocessThread.native_handle()) {
            SetThreadPriority((HANDLE)m_preprocessThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
        if (m_encodingThread.native_handle()) {
            SetThreadPriority((HANDLE)m_encodingThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
    } else {
        std::cout << "Starting sequential encoding pipeline..." << std::endl;
        
        // Start just the encoding thread for backward compatibility
        m_encodingThread = std::thread(&Encoder::EncodingThreadFunc, this);
    }
}

// Stop all pipeline threads
void Encoder::StopPipeline() {
    if (!m_running) {
        return; // Already stopped
    }
    
    std::cout << "Stopping encoding pipeline..." << std::endl;
    m_running = false;
    
    // Notify all condition variables to wake up threads
    m_rawQueueCV.notify_all();
    m_yuvQueueCV.notify_all();
    m_queueCV.notify_all();
    
    // Join threads
    if (m_preprocessThread.joinable()) {
        m_preprocessThread.join();
    }
    
    if (m_encodingThread.joinable()) {
        m_encodingThread.join();
    }
    
    std::cout << "Encoding pipeline stopped" << std::endl;
}

// Update the preprocessing thread function to handle frame pointers
void Encoder::PreprocessThreadFunc() {
    std::cout << "YUV conversion thread started" << std::endl;
    
    // Set thread affinity to the second core (index 1)
    SetThreadAffinityMask((HANDLE)m_preprocessThread.native_handle(), (1 << 1));
    
    // Stats for conversion
    int framesConverted = 0;
    LARGE_INTEGER lastConversionStatsTime;
    QueryPerformanceCounter(&lastConversionStatsTime);
    
    // For tracking conversion time
    LARGE_INTEGER conversionStart, conversionEnd;
    double totalConversionTime = 0.0;
    
    // Prefetch buffer size to improve cache locality
    const size_t PREFETCH_SIZE = 64; // Typical cache line size
    
    while (m_running) {
        if (m_bufferMode == BufferSystemMode::RingBuffer) {
            // Ring buffer implementation
            std::shared_ptr<EncoderFrame> frame;
            if (m_rawFrameBuffer->pop(frame, true)) {
                // Start timing conversion
                QueryPerformanceCounter(&conversionStart);
                
                // Convert BGRA to YUV
                bool success = false;
                if (frame->sharedData) {
                    // Create a temporary local EncoderFrame for conversion
                    EncoderFrame localFrame;
                    localFrame.width = frame->width;
                    localFrame.height = frame->height;
                    
                    // Convert using the YUV converter
                    success = ConvertBGRAtoYUV(frame->sharedData, localFrame);
                    
                    if (success) {
                        // Transfer the YUV data to the shared frame
                        frame->yPlane = localFrame.yPlane;
                        frame->uPlane = localFrame.uPlane;
                        frame->vPlane = localFrame.vPlane;
                        frame->yStride = localFrame.yStride;
                        frame->uStride = localFrame.uStride;
                        frame->vStride = localFrame.vStride;
                        frame->yuvConverted = true;
                    }
                }
                
                // End timing conversion
                QueryPerformanceCounter(&conversionEnd);
                double conversionTime = static_cast<double>(conversionEnd.QuadPart - conversionStart.QuadPart) / 
                                      static_cast<double>(m_frequency.QuadPart);
                totalConversionTime += conversionTime;
                
                if (success) {
                    // Push to YUV buffer for encoding
                    if (!m_yuvFrameBuffer->push(frame, false)) {
                        // Buffer full - frame dropped
                        std::cout << "WARNING: YUV buffer full, frame dropped" << std::endl;
                    }
                    
                    framesConverted++;
                }
                
                // Update conversion rate stats periodically
                LARGE_INTEGER currentTime;
                QueryPerformanceCounter(&currentTime);
                double elapsedSeconds = static_cast<double>(currentTime.QuadPart - lastConversionStatsTime.QuadPart) 
                                      / static_cast<double>(m_frequency.QuadPart);
                
                if (elapsedSeconds >= 1.0) {
                    m_pipelineStats.conversionRate = static_cast<float>(framesConverted) / static_cast<float>(elapsedSeconds);
                    
                    if (framesConverted > 0) {
                        double avgConversionTime = totalConversionTime / framesConverted * 1000.0; // in ms
                        std::cout << "YUV conversion: " << m_pipelineStats.conversionRate << " fps, avg time: " 
                                  << avgConversionTime << " ms" << std::endl;
                    }
                    
                    framesConverted = 0;
                    totalConversionTime = 0.0;
                    lastConversionStatsTime = currentTime;
                }
            }
        }
        else {
            // Original queue-based implementation
            EncoderFrame* frame = nullptr;
            bool frameAvailable = false;
            
            // Get a frame from the raw queue
            {
                std::unique_lock<std::mutex> lock(m_rawQueueMutex);
                
                // Wait for a frame or shutdown signal with a timeout to prevent deadlocks
                auto waitResult = m_rawQueueCV.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !m_rawFrameQueue.empty() || !m_running;
                });
                
                // Check if we should exit
                if (!m_running && m_rawFrameQueue.empty()) {
                    break;
                }
                
                // Get next frame
                if (!m_rawFrameQueue.empty()) {
                    frame = m_rawFrameQueue.front();
                    m_rawFrameQueue.pop();
                    frameAvailable = true;
                }
            }
            
            // Process the frame if we have one
            if (frameAvailable && frame) {
                // Start timing conversion
                QueryPerformanceCounter(&conversionStart);
                
                // Prefetch first few bytes of frame data to improve cache locality
                if (frame->sharedData && frame->sharedData->size() > 0) {
                    _mm_prefetch(reinterpret_cast<const char*>(frame->sharedData->data()), _MM_HINT_T0);
                    if (frame->sharedData->size() > PREFETCH_SIZE) {
                        _mm_prefetch(reinterpret_cast<const char*>(frame->sharedData->data() + PREFETCH_SIZE), _MM_HINT_T0);
                    }
                } else if (!frame->data.empty()) {
                    _mm_prefetch(reinterpret_cast<const char*>(frame->data.data()), _MM_HINT_T0);
                    if (frame->data.size() > PREFETCH_SIZE) {
                        _mm_prefetch(reinterpret_cast<const char*>(frame->data.data() + PREFETCH_SIZE), _MM_HINT_T0);
                    }
                }
                
                // Convert BGRA to YUV
                bool success = false;
                if (frame->sharedData) {
                    success = ConvertBGRAtoYUV(*frame->sharedData, *frame);
                } else {
                    success = ConvertBGRAtoYUV(frame->data, *frame);
                }
                
                // End timing conversion
                QueryPerformanceCounter(&conversionEnd);
                double conversionTime = static_cast<double>(conversionEnd.QuadPart - conversionStart.QuadPart) / 
                                      static_cast<double>(m_frequency.QuadPart);
                totalConversionTime += conversionTime;
                
                if (success) {
                    // Calculate frame latency from capture to conversion completion
                    double frameLatency = static_cast<double>(conversionEnd.QuadPart - frame->qpcTimestamp) / 
                                         static_cast<double>(m_frequency.QuadPart) * 1000.0; // in ms
                    
                    // Add converted frame to YUV queue
                    {
                        std::lock_guard<std::mutex> lock(m_yuvQueueMutex);
                        m_yuvFrameQueue.push(frame);
                        
                        // Monitor queue size for potential bottlenecks
                        if (m_yuvFrameQueue.size() > 5) {
                            std::cout << "Warning: YUV queue growing: " << m_yuvFrameQueue.size() 
                                      << " frames, conversion time: " << (conversionTime * 1000.0) << " ms" << std::endl;
                        }
                    }
                    
                    // Notify encoding thread
                    m_yuvQueueCV.notify_one();
                    
                    // Update stats
                    framesConverted++;
                } else {
                    // Release the frame back to the pool on error
                    m_framePool.ReleaseFrame(frame);
                    std::cerr << "Failed to convert frame to YUV" << std::endl;
                }
            }
        }
    }
    
    std::cout << "YUV conversion thread stopped" << std::endl;
}

// Update the encoding thread function to handle frame pointers
void Encoder::EncodingThreadFunc() {
    std::cout << "Encoding thread started" << std::endl;
    
    // Set thread affinity to the third core (index 2)
    SetThreadAffinityMask((HANDLE)m_encodingThread.native_handle(), (1 << 2));
    
    // Initialize stats tracking
    QueryPerformanceCounter(&m_lastStatsTime);
    
    // Stats for thread
    int framesEncoded = 0;
    LARGE_INTEGER encodingStart, encodingEnd;
    double totalEncodingTime = 0.0;
    double maxEncodingTime = 0.0;
    double minEncodingTime = 1000.0; // Initialize to a high value
    
    // Frame size stats for bitrate calculation
    int64_t totalEncodedSize = 0;
    int keyFrames = 0;
    
    // For more accurate frame time tracking
    double averageLatency = 0.0;
    double maxLatency = 0.0;
    
    // Frame duplication for ring buffer
    std::shared_ptr<EncoderFrame> lastFrame;
    int nextFrameNumber = 0;
    
    while (m_running) {
        if (m_bufferMode == BufferSystemMode::RingBuffer) {
            // Ring buffer implementation with fixed timing
            
            // Calculate time until next frame should be processed
            int delay = m_clock.calculateDelay(nextFrameNumber);
            
            if (delay > 0) {
                // We're ahead of schedule, wait
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            
            // Try to get a frame from the YUV buffer (non-blocking)
            std::shared_ptr<EncoderFrame> frame;
            bool hasFrame = m_yuvFrameBuffer->pop(frame, false);
            
            if (hasFrame) {
                // Start timing encoding
                QueryPerformanceCounter(&encodingStart);
                
                // Set frame's presentation timestamp based on frame number
                double pts = m_clock.frameToPts(nextFrameNumber);
                
                // Update x264 picture planes with YUV data
                m_picIn.img.plane[0] = frame->yPlane;
                m_picIn.img.plane[1] = frame->uPlane;
                m_picIn.img.plane[2] = frame->vPlane;
                m_picIn.img.i_stride[0] = frame->yStride;
                m_picIn.img.i_stride[1] = frame->uStride;
                m_picIn.img.i_stride[2] = frame->vStride;
                
                // Set PTS based on the frame number for consistent timing
                m_picIn.i_pts = nextFrameNumber;
                m_picIn.i_dts = m_picIn.i_pts;
                
                // Encode the frame
                x264_nal_t* nals;
                int i_nals;
                int frameSize = x264_encoder_encode(m_encoder, &nals, &i_nals, &m_picIn, &m_picOut);
                
                if (frameSize > 0) {
                    // Track if this is a keyframe
                    frame->isKeyFrame = m_picOut.b_keyframe != 0;
                    
                    // Write to file and call callback
                    if (m_outputFile.is_open()) {
                        m_outputFile.write(reinterpret_cast<char*>(nals[0].p_payload), frameSize);
                    }
                    
                    // Call callback if registered
                    if (m_callback) {
                        m_callback(nals[0].p_payload, frameSize, frame->isKeyFrame);
                    }
                    
                    // Update stats
                    m_encodedBytes += frameSize;
                    framesEncoded++;
                }
                
                // End encoding timing
                QueryPerformanceCounter(&encodingEnd);
                double encodingTime = static_cast<double>(encodingEnd.QuadPart - encodingStart.QuadPart) /
                                     static_cast<double>(m_frequency.QuadPart);
                
                totalEncodingTime += encodingTime;
                maxEncodingTime = std::max(maxEncodingTime, encodingTime);
                minEncodingTime = std::min(minEncodingTime, encodingTime);
                
                // Store as last frame for frame duplication if needed
                lastFrame = frame;
            }
            else if (lastFrame) {
                // No new frame available, duplicate the last one
                // Start timing encoding
                QueryPerformanceCounter(&encodingStart);
                
                // Use the last frame's YUV data
                m_picIn.img.plane[0] = lastFrame->yPlane;
                m_picIn.img.plane[1] = lastFrame->uPlane;
                m_picIn.img.plane[2] = lastFrame->vPlane;
                m_picIn.img.i_stride[0] = lastFrame->yStride;
                m_picIn.img.i_stride[1] = lastFrame->uStride;
                m_picIn.img.i_stride[2] = lastFrame->vStride;
                
                // Set PTS based on the frame number for consistent timing
                m_picIn.i_pts = nextFrameNumber;
                m_picIn.i_dts = m_picIn.i_pts;
                
                // Encode the frame
                x264_nal_t* nals;
                int i_nals;
                int frameSize = x264_encoder_encode(m_encoder, &nals, &i_nals, &m_picIn, &m_picOut);
                
                if (frameSize > 0) {
                    // Write to file and call callback
                    if (m_outputFile.is_open()) {
                        m_outputFile.write(reinterpret_cast<char*>(nals[0].p_payload), frameSize);
                    }
                    
                    // Call callback if registered
                    if (m_callback) {
                        bool isKeyFrame = m_picOut.b_keyframe != 0;
                        m_callback(nals[0].p_payload, frameSize, isKeyFrame);
                    }
                    
                    // Update stats
                    m_encodedBytes += frameSize;
                    framesEncoded++;
                    
                    std::cout << "Duplicated frame for smooth playback" << std::endl;
                }
                
                // End encoding timing
                QueryPerformanceCounter(&encodingEnd);
                double encodingTime = static_cast<double>(encodingEnd.QuadPart - encodingStart.QuadPart) /
                                     static_cast<double>(m_frequency.QuadPart);
                
                totalEncodingTime += encodingTime;
            }
            else {
                // No frames at all yet
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            
            // Increment frame number for next frame
            nextFrameNumber++;
        }
        else if (m_pipelineMode == EncoderPipelineMode::Parallel) {
            // Original queue-based implementation with parallel pipeline
            EncoderFrame* frame = nullptr;
            bool frameAvailable = false;
            
            {
                std::unique_lock<std::mutex> lock(m_yuvQueueMutex);
                
                // Wait for a frame or shutdown signal with timeout
                auto waitResult = m_yuvQueueCV.wait_for(lock, std::chrono::milliseconds(5), [this] {
                    return !m_yuvFrameQueue.empty() || !m_running;
                });
                
                // Check if we should exit
                if (!m_running && m_yuvFrameQueue.empty()) {
                    break;
                }
                
                // Get next frame
                if (!m_yuvFrameQueue.empty()) {
                    frame = m_yuvFrameQueue.front();
                    m_yuvFrameQueue.pop();
                    frameAvailable = true;
                }
            }
            
            // Process the frame if we have one
            if (frameAvailable && frame) {
                // Start timing encoding
                QueryPerformanceCounter(&encodingStart);
                
                // For latency calculation
                LARGE_INTEGER currentQPC;
                QueryPerformanceCounter(&currentQPC);
                double frameLatency = static_cast<double>(currentQPC.QuadPart - frame->qpcTimestamp) /
                                    static_cast<double>(m_frequency.QuadPart) * 1000.0; // Convert to ms
                
                // Track latency stats
                averageLatency = (averageLatency * framesEncoded + frameLatency) / (framesEncoded + 1);
                maxLatency = std::max(maxLatency, frameLatency);
                
                // Update pipeline stats
                m_pipelineStats.endToEndLatencyMs = static_cast<float>(frameLatency);
                
                // Use the pre-converted YUV data in the frame
                if (frame->yuvConverted) {
                    // Update picture planes with converted data
                    m_picIn.img.plane[0] = frame->yPlane;
                    m_picIn.img.plane[1] = frame->uPlane;
                    m_picIn.img.plane[2] = frame->vPlane;
                    m_picIn.img.i_stride[0] = frame->yStride;
                    m_picIn.img.i_stride[1] = frame->uStride;
                    m_picIn.img.i_stride[2] = frame->vStride;
                    
                    // For fixed frame rate encoding with frame timing buffer
                    // We're using a stable frame rate from the buffer, so 
                    // just increment frame counter for consistent timing
                    m_picIn.i_pts = m_frameCount;
                    m_picIn.i_dts = m_picIn.i_pts;
                    m_frameCount++;
                    
                    // Encode frame
                    x264_nal_t* nals;
                    int i_nals;
                    int frameSize = x264_encoder_encode(m_encoder, &nals, &i_nals, &m_picIn, &m_picOut);
                    
                    if (frameSize > 0) {
                        // Track keyframes and size
                        bool isKeyFrame = m_picOut.b_keyframe != 0;
                        if (isKeyFrame) {
                            keyFrames++;
                        }
                        
                        totalEncodedSize += frameSize;
                        
                        // Write to file and call callback
                        if (m_outputFile.is_open()) {
                            m_outputFile.write(reinterpret_cast<char*>(nals[0].p_payload), frameSize);
                        }
                        
                        // Call callback if registered
                        if (m_callback) {
                            m_callback(nals[0].p_payload, frameSize, isKeyFrame);
                        }
                        
                        // Update stats
                        m_encodedBytes += frameSize;
                        framesEncoded++;
                    }
                } else {
                    std::cerr << "Frame not converted to YUV" << std::endl;
                }
                
                // End encoding timing
                QueryPerformanceCounter(&encodingEnd);
                double encodingTime = static_cast<double>(encodingEnd.QuadPart - encodingStart.QuadPart) /
                                     static_cast<double>(m_frequency.QuadPart);
                
                totalEncodingTime += encodingTime;
                maxEncodingTime = std::max(maxEncodingTime, encodingTime);
                minEncodingTime = std::min(minEncodingTime, encodingTime);
                
                // Release the YUV buffer back to the pool
                if (frame->yuvConverted) {
                    m_yuvConverter->ReleaseYuvBuffer();
                }
                
                // Release the frame back to the pool
                m_framePool.ReleaseFrame(frame);
            } else {
                // No frame to process right now, just yield for a moment to reduce CPU usage
                if (m_useTimingBuffer) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        } else {
            // Original queue-based implementation with sequential pipeline
            EncoderFrame frame;
            bool shouldProcess = false;
            
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                
                if (!m_frameQueue.empty()) {
                    // Check if it's time to process the next frame
                    if (ShouldReleaseFrame(nullptr)) {
                        // Dequeue the frame
                        frame = m_frameQueue.front();
                        m_frameQueue.pop();
                        shouldProcess = true;
                        
                        // Update timing buffer
                        UpdateTimingBuffer();
                    } else {
                        // Not time yet, wait a bit
                        lock.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                } else {
                    // No frames available, wait for notification
                    m_queueCV.wait_for(lock, std::chrono::milliseconds(5), [this] {
                        return !m_frameQueue.empty() || !m_running;
                    });
                    
                    // Check if we should exit
                    if (!m_running && m_frameQueue.empty()) {
                        break;
                    }
                }
            }
            
            // Process the frame if we have one
            if (shouldProcess) {
                // For latency calculation
                auto now = std::chrono::high_resolution_clock::now();
                auto currentTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                float frameLatency = static_cast<float>(currentTimestamp - frame.timestamp);
                
                // Process using the original method for backward compatibility
                if (EncodeFrameInternal(frame)) {
                    framesEncoded++;
                    
                    // Update latency tracking
                    m_pipelineStats.endToEndLatencyMs = frameLatency;
                }
            } else if (m_useTimingBuffer) {
                // No frame to process, just yield to reduce CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        
        // Update stats periodically
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        double elapsedSeconds = static_cast<double>(currentTime.QuadPart - m_lastStatsTime.QuadPart) 
                               / static_cast<double>(m_frequency.QuadPart);
        
        if (elapsedSeconds >= 1.0) {
            // Calculate FPS and queue sizes
            m_actualFps = static_cast<float>(framesEncoded) / static_cast<float>(elapsedSeconds);
            m_pipelineStats.encodingRate = m_actualFps;
            
            int rawQueueSize = 0;
            int yuvQueueSize = 0;
            
            {
                std::lock_guard<std::mutex> lock(m_rawQueueMutex);
                rawQueueSize = static_cast<int>(m_rawFrameQueue.size());
            }
            
            {
                std::lock_guard<std::mutex> lock(m_yuvQueueMutex);
                yuvQueueSize = static_cast<int>(m_yuvFrameQueue.size());
            }
            
            // Calculate actual bitrate
            double actualBitrate = 0.0;
            if (framesEncoded > 0) {
                actualBitrate = (totalEncodedSize * 8.0) / elapsedSeconds / 1000.0; // kbps
            }
            
            // Calculate average encoding time
            double avgEncodingTime = 0.0;
            if (framesEncoded > 0) {
                avgEncodingTime = totalEncodingTime / framesEncoded * 1000.0; // ms
            }
            
            // Call stats callback if registered
            if (m_statsCallback) {
                m_statsCallback(m_actualFps, m_pipelineMode == EncoderPipelineMode::Parallel ? yuvQueueSize : 0);
            }
            
            // Log detailed stats to console
            std::cout << "Encoder stats: " 
                     << m_actualFps << " fps, "
                     << actualBitrate << " kbps, "
                     << "Raw queue: " << rawQueueSize << ", "
                     << "YUV queue: " << yuvQueueSize << ", "
                     << "Avg time: " << avgEncodingTime << " ms, "
                     << "Min/Max: " << (minEncodingTime * 1000.0) << "/" << (maxEncodingTime * 1000.0) << " ms, "
                     << "Latency: " << averageLatency << " ms, " 
                     << "Max latency: " << maxLatency << " ms, "
                     << "Key frames: " << keyFrames
                     << std::endl;
            
            // Reset stats
            framesEncoded = 0;
            m_lastStatsTime = currentTime;
            totalEncodingTime = 0.0;
            maxEncodingTime = 0.0;
            minEncodingTime = 1000.0;
            totalEncodedSize = 0;
            keyFrames = 0;
            averageLatency = 0.0;
            maxLatency = 0.0;
        }
    }
    
    std::cout << "Encoding thread stopped" << std::endl;
}

// Add conversion method for shared_ptr version
bool Encoder::ConvertBGRAtoYUV(const std::shared_ptr<std::vector<uint8_t>>& bgraFrame, EncoderFrame& frame) {
    // Check shared frame size
    if (!bgraFrame || bgraFrame->size() < frame.width * frame.height * 4) {
        std::cerr << "Input frame too small for resolution: " 
                  << (bgraFrame ? bgraFrame->size() : 0) << " vs " << (frame.width * frame.height * 4) << std::endl;
        return false;
    }
    
    // Convert BGRA to I420 using libyuv
    bool success = m_yuvConverter->ConvertBGRAtoI420(
        bgraFrame->data(),          // BGRA data
        frame.width * 4,           // BGRA stride
        frame.width, frame.height, // Dimensions
        &frame.yPlane, &frame.uPlane, &frame.vPlane, // Output planes
        &frame.yStride, &frame.uStride, &frame.vStride // Output strides
    );
    
    if (success) {
        frame.yuvConverted = true;
    } else {
        std::cerr << "YUV conversion failed" << std::endl;
    }
    
    return success;
}

// Implementation for the vector version that takes an EncoderFrame reference
bool Encoder::ConvertBGRAtoYUV(const std::vector<uint8_t>& bgraFrame, EncoderFrame& frame) {
    if (bgraFrame.size() < frame.width * frame.height * 4) {
        std::cerr << "Input frame too small for resolution: " 
                  << bgraFrame.size() << " vs " << (frame.width * frame.height * 4) << std::endl;
        return false;
    }
    
    // Convert BGRA to I420 using libyuv
    bool success = m_yuvConverter->ConvertBGRAtoI420(
        bgraFrame.data(),          // BGRA data
        frame.width * 4,           // BGRA stride
        frame.width, frame.height, // Dimensions
        &frame.yPlane, &frame.uPlane, &frame.vPlane, // Output planes
        &frame.yStride, &frame.uStride, &frame.vStride // Output strides
    );
    
    if (success) {
        frame.yuvConverted = true;
    } else {
        std::cerr << "YUV conversion failed" << std::endl;
    }
    
    return success;
}

// Get pipeline statistics
Encoder::PipelineStats Encoder::GetPipelineStats() const {
    PipelineStats stats;
    stats.captureRate = m_pipelineStats.captureRate;
    stats.conversionRate = m_pipelineStats.conversionRate;
    stats.encodingRate = m_pipelineStats.encodingRate;
    
    // Get current queue sizes
    int rawQueueSize = 0;
    int yuvQueueSize = 0;
    
    {
        const_cast<Encoder*>(this)->m_rawQueueMutex.lock();
        rawQueueSize = m_rawFrameQueue.size();
        const_cast<Encoder*>(this)->m_rawQueueMutex.unlock();
    }
    
    {
        const_cast<Encoder*>(this)->m_yuvQueueMutex.lock();
        yuvQueueSize = m_yuvFrameQueue.size();
        const_cast<Encoder*>(this)->m_yuvQueueMutex.unlock();
    }
    
    stats.rawQueueSize = rawQueueSize;
    stats.yuvQueueSize = yuvQueueSize;
    stats.endToEndLatencyMs = m_pipelineStats.endToEndLatencyMs;
    
    return stats;
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
    // Check for zero-copy mode (when frame.data is empty but sharedData is present)
    bool isZeroCopy = frame.data.empty() && frame.sharedData && !frame.sharedData->empty();
    
    // Debug output for frame size verification
    if (isZeroCopy) {
        size_t expectedSize = frame.width * frame.height * 4; // BGRA is 4 bytes per pixel
        if (frame.sharedData->size() < expectedSize) {
            std::cerr << "EncodeFrameInternal: Shared frame buffer too small. "
                      << "Expected: " << expectedSize << " bytes, "
                      << "Got: " << frame.sharedData->size() << " bytes" << std::endl;
            return false;
        }
    } else {
        size_t expectedSize = frame.width * frame.height * 4; // BGRA is 4 bytes per pixel
        if (frame.data.size() < expectedSize) {
            std::cerr << "EncodeFrameInternal: Frame buffer too small. "
                      << "Expected: " << expectedSize << " bytes, "
                      << "Got: " << frame.data.size() << " bytes" << std::endl;
            return false;
        }
    }
    
    // Convert BGRA to YUV420P (I420)
    bool conversionSuccess = false;
    if (isZeroCopy) {
        // Use the shared data for zero-copy mode
        conversionSuccess = ConvertBGRAtoYUV(*frame.sharedData);
    } else {
        // Use the regular data buffer
        conversionSuccess = ConvertBGRAtoYUV(frame.data);
    }
    
    if (!conversionSuccess) {
        std::cerr << "Failed to convert BGRA to YUV" << std::endl;
        return false;
    }
    
    // Using the frame buffering system to ensure consistent timing
    // With our timing buffer, frames are released at a consistent rate
    // regardless of capture rate fluctuations
    
    // Use monotonic frame counter for PTS values
    // This ensures each frame has exactly the right duration (1/fps seconds)
    m_picIn.i_pts = m_frameCount;
    
    // DTS should equal PTS for I/P frames since we don't use B-frames
    m_picIn.i_dts = m_picIn.i_pts;
    
    // Increment frame counter for next frame
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
        std::cout << " PTS: " << m_picIn.i_pts << std::endl;
    }
    
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

// Original ConvertBGRAtoYUV method (for backward compatibility)
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

// Initialize timing buffer with fps
void Encoder::InitializeTimingBuffer(int fps) {
    m_targetFrameInterval = 1000.0 / fps; // milliseconds between frames
    m_lastReleaseTime = std::chrono::steady_clock::now();
    std::cout << "Frame timing buffer initialized with " << fps << " fps "
              << "(interval: " << m_targetFrameInterval << " ms)" << std::endl;
}

// Determine if a frame should be released for encoding based on timing
bool Encoder::ShouldReleaseFrame(const EncoderFrame* frame) {
    if (!m_useTimingBuffer) {
        return true; // Always release frames when timing buffer is disabled
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastReleaseTime).count();
    
    // Release frame if enough time has passed since the last frame
    return elapsed >= m_targetFrameInterval;
}

// Update timing buffer after releasing a frame
void Encoder::UpdateTimingBuffer() {
    m_lastReleaseTime = std::chrono::steady_clock::now();
} 