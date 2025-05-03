#pragma once

#include "video_frame.h"
#include <vector>
#include <queue>
#include <mutex>
#include <memory>

/**
 * FramePool manages a pool of pre-allocated video frames for efficient memory reuse.
 * This approach avoids expensive allocations during capture and reduces memory fragmentation.
 */
class FramePool {
public:
    /**
     * Creates a frame pool with the specified capacity and frame dimensions.
     * @param capacity Number of frames to pre-allocate
     * @param width Frame width
     * @param height Frame height
     * @param allocateYuv Whether to pre-allocate YUV buffers as well
     */
    FramePool(size_t capacity, int width, int height, bool allocateYuv = false)
        : m_width(width), m_height(height), m_allocateYuv(allocateYuv) {
        
        // Pre-allocate frames
        for (size_t i = 0; i < capacity; i++) {
            auto frame = createFrame();
            m_availableFrames.push(frame);
        }
    }
    
    /**
     * Gets a frame from the pool or creates a new one if none are available.
     * @return A shared pointer to a video frame
     */
    std::shared_ptr<VideoFrame> getFrame() {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (m_availableFrames.empty()) {
            // Create a new frame if pool is empty
            return createFrame();
        }
        
        // Get frame from pool
        auto frame = m_availableFrames.front();
        m_availableFrames.pop();
        
        // Reset frame state but keep allocated memory
        frame->reset();
        
        return frame;
    }
    
    /**
     * Returns a frame to the pool for reuse.
     * @param frame The frame to return to the pool
     */
    void returnFrame(std::shared_ptr<VideoFrame> frame) {
        if (!frame) return;
        
        std::lock_guard<std::mutex> lock(m_mutex);
        m_availableFrames.push(frame);
    }
    
    /**
     * Gets the number of frames currently available in the pool.
     * @return Available frame count
     */
    size_t availableCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_availableFrames.size();
    }
    
    /**
     * Changes the frame dimensions for newly created frames.
     * @param width New frame width
     * @param height New frame height
     */
    void setFrameDimensions(int width, int height) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_width = width;
        m_height = height;
    }

private:
    /**
     * Creates a new pre-allocated frame.
     * @return A shared pointer to the new frame
     */
    std::shared_ptr<VideoFrame> createFrame() {
        auto frame = std::make_shared<VideoFrame>(m_width, m_height);
        
        // Allocate RGB/BGRA buffer
        size_t dataSize = VideoFrame::calculateDataSize(m_width, m_height);
        frame->allocateData(dataSize);
        
        // Optionally allocate YUV buffer
        if (m_allocateYuv) {
            size_t yuvSize = VideoFrame::calculateYuvSize(m_width, m_height);
            frame->allocateYuvData(yuvSize);
        }
        
        return frame;
    }

private:
    int m_width;
    int m_height;
    bool m_allocateYuv;
    std::queue<std::shared_ptr<VideoFrame>> m_availableFrames;
    mutable std::mutex m_mutex;
}; 