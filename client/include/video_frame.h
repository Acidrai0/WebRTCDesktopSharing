#pragma once

#include <memory>
#include <vector>
#include <cstdint>

/**
 * VideoFrame represents a single frame in the capture and encoding pipeline.
 * It uses shared pointers for efficient memory management and zero-copy operations.
 */
class VideoFrame {
public:
    // Frame data - BGR/BGRA format
    std::shared_ptr<std::vector<uint8_t>> data;
    
    // YUV data (after conversion)
    std::shared_ptr<std::vector<uint8_t>> yuvData;
    std::vector<uint8_t> rawYuvData;  // Non-shared YUV data buffer
    
    // Frame dimensions
    int width = 0;
    int height = 0;
    
    // Timing information
    int64_t captureTimestamp = 0;  // When frame was captured (QPC)
    int64_t pts = 0;               // Presentation timestamp
    int frameNumber = 0;           // Frame sequence number
    
    // Frame type and flags
    bool isKeyFrame = false;
    bool isConverted = false;
    
    /**
     * Default constructor
     */
    VideoFrame() = default;
    
    /**
     * Constructor with frame dimensions
     */
    VideoFrame(int width, int height) 
        : width(width), height(height) {
    }
    
    /**
     * Allocates memory for the RGB/BGRA data buffer.
     * @param size Buffer size in bytes
     */
    void allocateData(size_t size) {
        data = std::make_shared<std::vector<uint8_t>>(size);
    }
    
    /**
     * Allocates memory for the YUV data buffer.
     * @param size Buffer size in bytes
     */
    void allocateYuvData(size_t size) {
        yuvData = std::make_shared<std::vector<uint8_t>>(size);
    }
    
    /**
     * Resets the frame for reuse without deallocating memory.
     */
    void reset() {
        // Keep buffers but reset metadata
        width = 0;
        height = 0;
        captureTimestamp = 0;
        pts = 0;
        frameNumber = 0;
        isKeyFrame = false;
        isConverted = false;
    }
    
    /**
     * Calculates the required size for the RGB/BGRA buffer.
     */
    static size_t calculateDataSize(int width, int height, int bytesPerPixel = 4) {
        return width * height * bytesPerPixel;
    }
    
    /**
     * Calculates the required size for the YUV buffer (I420 format).
     */
    static size_t calculateYuvSize(int width, int height) {
        // I420 format: Y plane (width*height) + U plane (width*height/4) + V plane (width*height/4)
        return width * height + (width * height / 2);
    }
}; 