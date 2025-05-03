#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <immintrin.h> // For SSE2/AVX intrinsics

// For CPU feature detection
#ifdef _WIN32
#include <intrin.h>
#else
#include <cpuid.h>
#endif

class SimdMemUtils {
public:
    // CPU features detection
    static bool HasSSE2();
    static bool HasAVX();
    static bool HasAVX2();
    
    // Initialize - automatically detect available CPU features
    static void Initialize();

    // Memory copy functions - optimized with SIMD
    static void CopyUnalignedToAligned(uint8_t* dst, const uint8_t* src, size_t size);
    
    // Specialized copy function for screen capture rows
    static void CopyRowWithStride(uint8_t* dst, const uint8_t* src, 
                                int width, int height, size_t srcStride);

private:
    static bool s_hasSSE2;
    static bool s_hasAVX;
    static bool s_hasAVX2;
    static bool s_initialized;

    // Implementation for different instruction sets
    // SSE2 implementations (always available on x64)
    static void CopyUnalignedToAligned_SSE2(uint8_t* dst, const uint8_t* src, size_t size);
    static void CopyRowWithStride_SSE2(uint8_t* dst, const uint8_t* src, 
                                      size_t row_width, size_t src_stride);
    
    // AVX implementations
    static void CopyUnalignedToAligned_AVX(uint8_t* dst, const uint8_t* src, size_t size);
    static void CopyRowWithStride_AVX(uint8_t* dst, const uint8_t* src, 
                                     size_t row_width, size_t src_stride);
    
    // Fallback standard implementations
    static void CopyUnalignedToAligned_Std(uint8_t* dst, const uint8_t* src, size_t size);
    static void CopyRowWithStride_Std(uint8_t* dst, const uint8_t* src, 
                                     size_t row_width, size_t src_stride);
};

// Static member initialization
bool SimdMemUtils::s_hasSSE2 = false;
bool SimdMemUtils::s_hasAVX = false;
bool SimdMemUtils::s_hasAVX2 = false;

// Check for SSE2 support (most modern CPUs have this)
bool SimdMemUtils::HasSSE2() {
    return s_hasSSE2;
}

// Check for AVX support
bool SimdMemUtils::HasAVX() {
    return s_hasAVX;
}

// Check for AVX2 support
bool SimdMemUtils::HasAVX2() {
    return s_hasAVX2;
}

// Initialize and detect CPU features
void SimdMemUtils::Initialize() {
    // Use CPUID to check CPU features
    int cpuInfo[4] = {0};
    
    // Check SSE2
    __cpuid(cpuInfo, 1);
    s_hasSSE2 = (cpuInfo[3] & (1 << 26)) != 0;
    
    // Check AVX
    s_hasAVX = (cpuInfo[2] & (1 << 28)) != 0;
    
    // Check AVX2
    if (s_hasAVX) {
        __cpuidex(cpuInfo, 7, 0);
        s_hasAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
    }
}

// Copy memory with SIMD optimizations
void SimdMemUtils::CopyUnalignedToAligned(uint8_t* dst, const uint8_t* src, size_t size) {
    // Use different SIMD instruction sets based on CPU capabilities
    if (s_hasAVX2 && size >= 32) {
        // AVX2 version (256-bit / 32-byte chunks)
        size_t i = 0;
        for (; i + 32 <= size; i += 32) {
            // Load 32 bytes (unaligned) from source
            __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
            // Store 32 bytes (aligned) to destination
            _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i), data);
        }
        
        // Handle remaining bytes with SSE2
        for (; i + 16 <= size; i += 16) {
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
            _mm_store_si128(reinterpret_cast<__m128i*>(dst + i), data);
        }
        
        // Handle tail with memcpy
        if (i < size) {
            memcpy(dst + i, src + i, size - i);
        }
    }
    else if (s_hasSSE2 && size >= 16) {
        // SSE2 version (128-bit / 16-byte chunks)
        size_t i = 0;
        for (; i + 16 <= size; i += 16) {
            // Load 16 bytes (unaligned) from source
            __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
            // Store 16 bytes (aligned) to destination
            _mm_store_si128(reinterpret_cast<__m128i*>(dst + i), data);
        }
        
        // Handle tail with memcpy
        if (i < size) {
            memcpy(dst + i, src + i, size - i);
        }
    }
    else {
        // Fallback to standard memcpy
        memcpy(dst, src, size);
    }
}

// Specialized function for screen capture row copying
void SimdMemUtils::CopyRowWithStride(uint8_t* dst, const uint8_t* src, 
                                   int width, int height, size_t srcStride) {
    // Calculate sizes
    const size_t bytesPerPixel = 4; // BGRA format
    const size_t dstStride = width * bytesPerPixel;
    const size_t rowSize = width * bytesPerPixel;
    
    // Process each row
    for (int y = 0; y < height; y++) {
        // Get pointers for this row
        uint8_t* dstRow = dst + y * dstStride;
        const uint8_t* srcRow = src + y * srcStride;
        
        // Copy row with SIMD
        CopyUnalignedToAligned(dstRow, srcRow, rowSize);
    }
} 