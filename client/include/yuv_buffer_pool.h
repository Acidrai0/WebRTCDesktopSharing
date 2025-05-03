#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <algorithm>
#include <iostream>

/**
 * YuvBuffer - Holds aligned memory for YUV planes
 */
class YuvBuffer {
public:
    uint8_t* y = nullptr;
    uint8_t* u = nullptr;
    uint8_t* v = nullptr;
    int width = 0;
    int height = 0;
    int yStride = 0;
    int uStride = 0;
    int vStride = 0;
    bool inUse = false;

    YuvBuffer() = default;
    
    ~YuvBuffer() {
        Deallocate();
    }

    void Allocate(int w, int h) {
        // Deallocate if already allocated
        Deallocate();

        width = w;
        height = h;
        
        // Calculate strides
        yStride = width;
        uStride = (width + 1) / 2;
        vStride = (width + 1) / 2;

        // Allocate aligned memory for best SIMD performance
        // Use 32-byte alignment for AVX
#ifdef _WIN32
        y = (uint8_t*)_aligned_malloc(yStride * height, 32);
        u = (uint8_t*)_aligned_malloc(uStride * ((height + 1) / 2), 32);
        v = (uint8_t*)_aligned_malloc(vStride * ((height + 1) / 2), 32);
#else
        posix_memalign((void**)&y, 32, yStride * height);
        posix_memalign((void**)&u, 32, uStride * ((height + 1) / 2));
        posix_memalign((void**)&v, 32, vStride * ((height + 1) / 2));
#endif
    }

    void Deallocate() {
        if (y) {
#ifdef _WIN32
            _aligned_free(y);
#else
            free(y);
#endif
            y = nullptr;
        }
        
        if (u) {
#ifdef _WIN32
            _aligned_free(u);
#else
            free(u);
#endif
            u = nullptr;
        }
        
        if (v) {
#ifdef _WIN32
            _aligned_free(v);
#else
            free(v);
#endif
            v = nullptr;
        }
    }
};

/**
 * YuvBufferPool - Manages a pool of YUV buffers for efficient memory reuse
 */
class YuvBufferPool {
public:
    YuvBufferPool(size_t maxPoolSize = 3) : m_maxPoolSize(maxPoolSize) {}
    
    ~YuvBufferPool() {
        // Buffers will clean up their memory
    }

    // Get a buffer from the pool or create a new one
    YuvBuffer* GetBuffer(int width, int height) {
        // First check if we have a matching unused buffer
        for (auto& buffer : m_buffers) {
            if (!buffer->inUse && buffer->width == width && buffer->height == height) {
                buffer->inUse = true;
                return buffer.get();
            }
        }

        // If no matching buffer, create a new one
        if (m_buffers.size() < m_maxPoolSize) {
            auto newBuffer = std::make_unique<YuvBuffer>();
            newBuffer->Allocate(width, height);
            newBuffer->inUse = true;
            
            m_buffers.push_back(std::move(newBuffer));
            return m_buffers.back().get();
        }

        // If we're here, we need to reuse an existing buffer of different size
        // Find the least recently used buffer
        auto oldestIt = std::find_if(m_buffers.begin(), m_buffers.end(),
            [](const std::unique_ptr<YuvBuffer>& buffer) {
                return !buffer->inUse;
            });
        
        // If all buffers are in use, create a new one anyway
        if (oldestIt == m_buffers.end()) {
            auto newBuffer = std::make_unique<YuvBuffer>();
            newBuffer->Allocate(width, height);
            newBuffer->inUse = true;
            
            m_buffers.push_back(std::move(newBuffer));
            return m_buffers.back().get();
        }
        
        // Reallocate if necessary
        YuvBuffer* buffer = oldestIt->get();
        if (buffer->width != width || buffer->height != height) {
            buffer->Allocate(width, height);
        }
        
        buffer->inUse = true;
        return buffer;
    }

    // Release a buffer back to the pool
    void ReleaseBuffer(YuvBuffer* buffer) {
        for (auto& buf : m_buffers) {
            if (buf.get() == buffer) {
                buf->inUse = false;
                break;
            }
        }
    }

private:
    std::vector<std::unique_ptr<YuvBuffer>> m_buffers;
    size_t m_maxPoolSize;
}; 