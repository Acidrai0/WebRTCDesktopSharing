#include "encoder.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <chrono>
#include <ctime>

Encoder::Encoder() : m_frameCount(0), m_encoder(nullptr) {
    std::cout << "Encoder created" << std::endl;
    
    // Initialize x264 picture structures
    x264_picture_init(&m_picIn);
    x264_picture_init(&m_picOut);
}

Encoder::~Encoder() {
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
    
    // Configure x264 parameters
    x264_param_t param;
    
    // Start with a preset for good quality and speed balance
    if (x264_param_default_preset(&param, "medium", "zerolatency") < 0) {
        std::cerr << "Failed to set x264 preset" << std::endl;
        return false;
    }
    
    // Configure basic settings
    param.i_width = width;
    param.i_height = height;
    param.i_fps_num = fps;
    param.i_fps_den = 1;
    param.i_csp = X264_CSP_I420;  // Use I420 colorspace
    
    // Set rate control
    param.rc.i_rc_method = X264_RC_ABR;  // Average bitrate
    param.rc.i_bitrate = bitrate / 1000;  // kilobits per second
    
    // Set keyframe interval (2 seconds)
    param.i_keyint_max = fps * 2;
    
    // Disable B-frames for low latency
    param.i_bframe = 0;
    
    // Use a single thread for now (can be optimized later)
    param.i_threads = 1;
    
    // Low latency settings
    param.b_repeat_headers = 1;  // Repeat SPS/PPS headers
    param.b_annexb = 1;  // Use Annex B format (NALU start codes)
    
    // Apply parameters and create encoder
    if (x264_param_apply_profile(&param, "baseline") < 0) {
        std::cerr << "Failed to apply x264 profile" << std::endl;
        return false;
    }
    
    // Create the encoder
    m_encoder = x264_encoder_open(&param);
    if (!m_encoder) {
        std::cerr << "Failed to open x264 encoder" << std::endl;
        return false;
    }
    
    // Allocate YUV buffer
    // I420 format: Y plane + U plane (1/4 size) + V plane (1/4 size)
    const size_t ySize = width * height;
    const size_t uvSize = (width / 2) * (height / 2);
    m_yuvBuffer.resize(ySize + uvSize * 2);
    
    // Set up input picture
    m_picIn.img.i_csp = X264_CSP_I420;
    m_picIn.img.i_plane = 3;  // Y, U, V planes
    
    // Set up plane pointers
    m_picIn.img.plane[0] = m_yuvBuffer.data();                      // Y plane
    m_picIn.img.plane[1] = m_picIn.img.plane[0] + ySize;            // U plane
    m_picIn.img.plane[2] = m_picIn.img.plane[1] + uvSize;           // V plane
    
    // Set up plane strides
    m_picIn.img.i_stride[0] = width;         // Y stride
    m_picIn.img.i_stride[1] = width / 2;     // U stride
    m_picIn.img.i_stride[2] = width / 2;     // V stride
    
    // Open output file
    OpenOutputFile();
    
    std::cout << "Encoder initialized with " << width << "x" << height 
              << " at " << fps << " fps and " << bitrate << " bps" << std::endl;
    
    return true;
}

bool Encoder::OpenOutputFile() {
    // Generate a filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char timestamp[20];
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", std::localtime(&time_t_now));
    
    // Create filename with timestamp and resolution
    std::string filename = "capture_" + std::string(timestamp) + "_" + 
                          std::to_string(m_width) + "x" + std::to_string(m_height) + ".h264";
    
    // Open the file
    m_outputFile.open(filename, std::ios::binary);
    if (!m_outputFile.is_open()) {
        std::cerr << "Failed to open output file: " << filename << std::endl;
        return false;
    }
    
    // Write SPS and PPS headers
    OutputHeaders();
    
    std::cout << "Recording to file: " << filename << std::endl;
    return true;
}

void Encoder::CloseOutputFile() {
    if (m_outputFile.is_open()) {
        m_outputFile.close();
        std::cout << "Recording stopped" << std::endl;
    }
}

void Encoder::OutputHeaders() {
    if (!m_encoder || !m_outputFile.is_open()) {
        return;
    }
    
    // Get SPS and PPS headers
    x264_nal_t* nals;
    int num_nals;
    x264_encoder_headers(m_encoder, &nals, &num_nals);
    
    // Write headers to file
    for (int i = 0; i < num_nals; i++) {
        m_outputFile.write(reinterpret_cast<const char*>(nals[i].p_payload), nals[i].i_payload);
    }
}

bool Encoder::ConvertBGRAtoYUV(const std::vector<uint8_t>& bgraFrame) {
    if (bgraFrame.size() < m_width * m_height * 4) {
        std::cerr << "BGRA buffer is too small for conversion" << std::endl;
        return false;
    }
    
    // Get pointers to the planes
    uint8_t* y_plane = m_picIn.img.plane[0];
    uint8_t* u_plane = m_picIn.img.plane[1];
    uint8_t* v_plane = m_picIn.img.plane[2];
    
    // Simple color conversion BGRA -> I420 (YUV)
    // This can be optimized with SIMD instructions or by using libyuv
    for (int y = 0; y < m_height; y++) {
        for (int x = 0; x < m_width; x++) {
            const int bgra_index = (y * m_width + x) * 4;
            const int y_index = y * m_width + x;
            
            // Extract BGRA values
            const uint8_t b = bgraFrame[bgra_index + 0];
            const uint8_t g = bgraFrame[bgra_index + 1];
            const uint8_t r = bgraFrame[bgra_index + 2];
            
            // Convert to Y (standard conversion formula)
            y_plane[y_index] = static_cast<uint8_t>(
                (0.257 * r) + (0.504 * g) + (0.098 * b) + 16);
            
            // Downsample and convert to U and V (2x2 pixels -> 1 U,V sample)
            if (y % 2 == 0 && x % 2 == 0) {
                const int uv_index = (y / 2) * (m_width / 2) + (x / 2);
                
                // Use the average of 4 pixels for U and V values
                uint8_t r_avg = r;
                uint8_t g_avg = g;
                uint8_t b_avg = b;
                
                // Simple conversion formula for U and V
                u_plane[uv_index] = static_cast<uint8_t>(
                    (-0.148 * r_avg) - (0.291 * g_avg) + (0.439 * b_avg) + 128);
                v_plane[uv_index] = static_cast<uint8_t>(
                    (0.439 * r_avg) - (0.368 * g_avg) - (0.071 * b_avg) + 128);
            }
        }
    }
    
    return true;
}

bool Encoder::EncodeFrame(const std::vector<uint8_t>& bgraFrame, int width, int height) {
    // Check if the frame dimensions match
    if (width != m_width || height != m_height) {
        std::cerr << "Frame dimensions don't match encoder settings" << std::endl;
        return false;
    }
    
    // Check if encoder is initialized
    if (!m_encoder) {
        std::cerr << "Encoder not initialized" << std::endl;
        return false;
    }
    
    // Convert BGRA to YUV (I420)
    if (!ConvertBGRAtoYUV(bgraFrame)) {
        std::cerr << "Failed to convert BGRA to YUV" << std::endl;
        return false;
    }
    
    // Set the input picture timestamp
    m_picIn.i_pts = m_frameCount;
    
    // Encode the frame
    x264_nal_t* nals;
    int num_nals;
    int frame_size = x264_encoder_encode(m_encoder, &nals, &num_nals, &m_picIn, &m_picOut);
    
    if (frame_size < 0) {
        std::cerr << "Failed to encode frame" << std::endl;
        return false;
    }
    
    // If we got encoded data
    if (frame_size > 0) {
        // Determine if this is a keyframe
        bool isKeyFrame = m_picOut.b_keyframe != 0;
        
        // Prepare a buffer for the combined NAL units
        std::vector<uint8_t> encodedData;
        encodedData.reserve(frame_size);
        
        // Combine all NAL units
        for (int i = 0; i < num_nals; i++) {
            encodedData.insert(encodedData.end(), 
                               nals[i].p_payload, 
                               nals[i].p_payload + nals[i].i_payload);
            
            // Write to output file if open
            if (m_outputFile.is_open()) {
                m_outputFile.write(reinterpret_cast<const char*>(nals[i].p_payload), 
                                   nals[i].i_payload);
            }
        }
        
        // Call the callback with the encoded data if set
        if (m_callback) {
            m_callback(encodedData.data(), encodedData.size(), isKeyFrame);
        }
        
        std::cout << "Encoding frame " << m_frameCount 
                  << " (" << width << "x" << height << ") "
                  << "size: " << encodedData.size() << " bytes"
                  << (isKeyFrame ? " [KEYFRAME]" : "")
                  << std::endl;
    }
    
    // Increment frame counter
    m_frameCount++;
    
    return true;
} 