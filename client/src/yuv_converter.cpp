#include "yuv_converter.h"
#include "yuv_buffer_pool.h"
#include <iostream>

// Include libyuv header
extern "C" {
    #include "libyuv.h"
}

// Helper to detect pixel format by examining bytes
std::string DetectPixelFormat(const uint8_t* pixelData) {
    // Extract channels from first pixel (assuming 4 bytes per pixel)
    uint8_t byte0 = pixelData[0];
    uint8_t byte1 = pixelData[1];
    uint8_t byte2 = pixelData[2];
    uint8_t byte3 = pixelData[3];
    
    // Format detection logic
    if (byte3 == 255 || byte3 == 0) {  // Alpha is usually 255 (opaque) or 0 (transparent)
        // Log the values
        std::cout << "Sample pixel bytes: [" 
                  << (int)byte0 << ", " 
                  << (int)byte1 << ", " 
                  << (int)byte2 << ", " 
                  << (int)byte3 << "]" << std::endl;
                  
        // Check if the sample has significant values to determine pattern
        bool hasBlue = byte0 > 30;
        bool hasGreen = byte1 > 30;
        bool hasRed = byte2 > 30;
        
        if (hasRed && !hasGreen && !hasBlue) {
            return "BGRA - Pure Red Sample";
        } else if (!hasRed && hasGreen && !hasBlue) {
            return "BGRA - Pure Green Sample";
        } else if (!hasRed && !hasGreen && hasBlue) {
            return "BGRA - Pure Blue Sample";
        } else if (hasRed && hasGreen && hasBlue) {
            // If 3 channels have content, try to analyze relative values
            if (byte0 > byte2 && byte1 > byte2) {
                return "BGRA - Bluish/Greenish dominance";
            } else if (byte2 > byte0 && byte1 > byte0) {
                return "BGRA - Reddish/Greenish dominance";
            } else if (byte2 > byte1 && byte0 > byte1) {
                return "BGRA - Reddish/Bluish dominance";
            } else {
                return "BGRA - Mixed color distribution";
            }
        } else {
            return "BGRA - Low color saturation";
        }
    }
    
    return "Unrecognized format";
}

YuvConverter::YuvConverter() 
    : m_bufferPool(std::make_unique<YuvBufferPool>()) {
    // Initialize with a default pool size of 3 buffers
}

YuvConverter::~YuvConverter() {
    // Release current buffer if in use
    if (m_currentBuffer && m_currentBuffer->inUse) {
        ReleaseYuvBuffer();
    }
    
    // Buffer pool will clean up automatically
}

bool YuvConverter::ConvertBGRAtoI420(
    const uint8_t* bgraData, 
    int bgraStride,
    int width, 
    int height,
    uint8_t** yPlane,
    uint8_t** uPlane, 
    uint8_t** vPlane,
    int* yStride,
    int* uStride,
    int* vStride
) {
    // Release previous buffer if still in use
    if (m_currentBuffer && m_currentBuffer->inUse) {
        ReleaseYuvBuffer();
    }
    
    // Get a buffer from the pool
    m_currentBuffer = m_bufferPool->GetBuffer(width, height);
    if (!m_currentBuffer) {
        std::cerr << "Failed to get YUV buffer from pool" << std::endl;
        return false;
    }
    
    // Set output pointers
    *yPlane = m_currentBuffer->y;
    *uPlane = m_currentBuffer->u;
    *vPlane = m_currentBuffer->v;
    *yStride = m_currentBuffer->yStride;
    *uStride = m_currentBuffer->uStride;
    *vStride = m_currentBuffer->vStride;
    
    // Using libyuv's ARGBToI420 function for conversion instead of BGRAToI420
    int result = libyuv::ARGBToI420(
        bgraData, bgraStride,
        *yPlane, *yStride,
        *uPlane, *uStride,
        *vPlane, *vStride,
        width, height
    );
    
    if (result != 0) {
        std::cerr << "libyuv conversion failed with error: " << result << std::endl;
        ReleaseYuvBuffer();
        return false;
    }
    
    return true;
}

void YuvConverter::ReleaseYuvBuffer() {
    if (m_currentBuffer) {
        m_bufferPool->ReleaseBuffer(m_currentBuffer);
        m_currentBuffer = nullptr;
    }
} 