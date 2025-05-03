#pragma once

#include <cstdint>
#include <vector>
#include <memory>

// Forward declarations
class YuvBuffer;
class YuvBufferPool;

/**
 * YuvConverter - Uses libyuv to efficiently convert RGB frames to YUV format
 * Provides buffer pooling and optimized memory management
 */
class YuvConverter {
public:
    YuvConverter();
    ~YuvConverter();

    /**
     * Convert BGRA data to I420 (YUV420P) format used by x264
     * 
     * @param bgraData RGB source data (BGRA format)
     * @param bgraStride Source stride in bytes
     * @param width Frame width
     * @param height Frame height
     * @param yPlane Pointer to receive Y plane pointer
     * @param uPlane Pointer to receive U plane pointer
     * @param vPlane Pointer to receive V plane pointer
     * @param yStride Pointer to receive Y stride
     * @param uStride Pointer to receive U stride
     * @param vStride Pointer to receive V stride
     * @return true if conversion succeeded
     */
    bool ConvertBGRAtoI420(
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
    );

    /**
     * Release a previously acquired YUV buffer back to the pool
     */
    void ReleaseYuvBuffer();

private:
    // YUV buffer pool for efficient memory reuse
    std::unique_ptr<YuvBufferPool> m_bufferPool;
    
    // Current active YUV buffer
    YuvBuffer* m_currentBuffer = nullptr;
}; 