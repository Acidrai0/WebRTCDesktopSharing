#pragma once

#include <fstream>
#include <string>
#include <chrono>
#include <mutex>
#include <Windows.h>
#include <psapi.h>

class PerformanceLogger {
public:
    static PerformanceLogger& GetInstance() {
        static PerformanceLogger instance;
        return instance;
    }

    bool Initialize(const std::string& filename) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_logFile.open(filename, std::ios::out | std::ios::trunc);
        
        if (!m_logFile.is_open()) {
            return false;
        }

        // Write header
        m_logFile << "Timestamp,Stage,FPS,Frame Count,Queue Size,Memory Usage (MB)\n";
        
        // Get QPC frequency
        QueryPerformanceFrequency(&m_frequency);
        
        return true;
    }

    void LogPerformance(const std::string& stage, float fps, int frameCount, int queueSize = -1) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_logFile.is_open()) {
            return;
        }

        // Get timestamp
        LARGE_INTEGER timestamp;
        QueryPerformanceCounter(&timestamp);
        double seconds = static_cast<double>(timestamp.QuadPart) / static_cast<double>(m_frequency.QuadPart);

        // Get memory usage
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            SIZE_T memoryUsageMB = pmc.WorkingSetSize / (1024 * 1024);
            
            m_logFile << seconds << "," << stage << "," << fps << "," << frameCount << "," 
                      << queueSize << "," << memoryUsageMB << std::endl;
        } else {
            // If memory info fails, log without memory usage
            m_logFile << seconds << "," << stage << "," << fps << "," << frameCount << "," 
                      << queueSize << "," << "N/A" << std::endl;
        }
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_logFile.is_open()) {
            m_logFile.close();
        }
    }

private:
    PerformanceLogger() {}
    ~PerformanceLogger() {
        Shutdown();
    }

    PerformanceLogger(const PerformanceLogger&) = delete;
    PerformanceLogger& operator=(const PerformanceLogger&) = delete;

    std::ofstream m_logFile;
    std::mutex m_mutex;
    LARGE_INTEGER m_frequency;
}; 