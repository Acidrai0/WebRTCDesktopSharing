#pragma once

#include <Windows.h>
#include <cstdint>
#include <chrono>

/**
 * GlobalClock provides a centralized timing mechanism for the capture pipeline.
 * It ensures consistent frame timing and PTS generation similar to FFmpeg's approach.
 */
class GlobalClock {
public:
    /**
     * Initializes the global clock with the specified frame rate.
     * @param fps Target frame rate
     */
    GlobalClock(double fps = 30.0) : m_fps(fps), m_timeBase(1.0 / fps) {
        // Initialize performance counter
        QueryPerformanceFrequency(&m_frequency);
        QueryPerformanceCounter(&m_startTime);
        
        // Initialize frame counter
        m_frameCount = 0;
        
        // Set timebase in QPC ticks
        m_timeBaseInTicks = static_cast<int64_t>(m_timeBase * static_cast<double>(m_frequency.QuadPart));
    }
    
    /**
     * Gets the current time in clock units (seconds since start).
     * @return Current time in seconds
     */
    double now() {
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        
        return static_cast<double>(currentTime.QuadPart - m_startTime.QuadPart) / 
               static_cast<double>(m_frequency.QuadPart);
    }
    
    /**
     * Gets the current time in QPC ticks since start.
     * @return Current time in QPC ticks
     */
    int64_t nowTicks() {
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        
        return currentTime.QuadPart - m_startTime.QuadPart;
    }
    
    /**
     * Converts a frame number to presentation timestamp.
     * @param frameNumber Frame sequence number
     * @return PTS value in seconds
     */
    double frameToPts(int frameNumber) {
        return frameNumber * m_timeBase;
    }
    
    /**
     * Converts a frame number to presentation timestamp in QPC ticks.
     * @param frameNumber Frame sequence number
     * @return PTS value in QPC ticks
     */
    int64_t frameToPtsTicks(int frameNumber) {
        return frameNumber * m_timeBaseInTicks;
    }
    
    /**
     * Gets the PTS for the next frame and increments the frame counter.
     * @return PTS for the next frame in seconds
     */
    double nextFramePts() {
        return frameToPts(m_frameCount++);
    }
    
    /**
     * Gets the PTS for the next frame in QPC ticks and increments the frame counter.
     * @return PTS for the next frame in QPC ticks
     */
    int64_t nextFramePtsTicks() {
        return frameToPtsTicks(m_frameCount++);
    }
    
    /**
     * Converts a time value to the nearest frame number.
     * @param time Time value in seconds
     * @return Nearest frame number
     */
    int timeToFrame(double time) {
        return static_cast<int>(time / m_timeBase + 0.5);
    }
    
    /**
     * Converts a time value in QPC ticks to the nearest frame number.
     * @param ticks Time value in QPC ticks
     * @return Nearest frame number
     */
    int ticksToFrame(int64_t ticks) {
        return static_cast<int>(static_cast<double>(ticks) / 
               static_cast<double>(m_timeBaseInTicks) + 0.5);
    }
    
    /**
     * Gets the current frame number based on elapsed time.
     * @return Current frame number
     */
    int currentFrame() {
        return timeToFrame(now());
    }
    
    /**
     * Gets the time duration between frames.
     * @return Time between frames in seconds
     */
    double frameInterval() const {
        return m_timeBase;
    }
    
    /**
     * Gets the target frame rate.
     * @return Target frame rate in FPS
     */
    double fps() const {
        return m_fps;
    }
    
    /**
     * Calculates the delay until the next frame should be processed.
     * @param frameNumber Target frame number
     * @return Delay in milliseconds (negative if we're behind)
     */
    int calculateDelay(int frameNumber) {
        double targetTime = frameToPts(frameNumber);
        double currentTime = now();
        double delay = targetTime - currentTime;
        
        // Convert to milliseconds
        return static_cast<int>(delay * 1000.0);
    }
    
    /**
     * Resets the clock to the current time.
     */
    void reset() {
        QueryPerformanceCounter(&m_startTime);
        m_frameCount = 0;
    }

private:
    LARGE_INTEGER m_frequency;     // Performance counter frequency
    LARGE_INTEGER m_startTime;     // Start time in QPC ticks
    double m_fps;                  // Target frame rate
    double m_timeBase;             // Time per frame in seconds
    int64_t m_timeBaseInTicks;     // Time per frame in QPC ticks
    int m_frameCount;              // Frame counter
}; 