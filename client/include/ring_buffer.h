#pragma once

#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <stdexcept>

/**
 * Thread-safe ring buffer implementation inspired by FFmpeg's buffer system.
 * This class provides a fixed-size circular buffer with blocking and non-blocking operations.
 */
template <typename T>
class RingBuffer {
public:
    /**
     * Constructs a ring buffer with the specified capacity.
     * @param capacity Maximum number of items the buffer can hold
     */
    explicit RingBuffer(size_t capacity)
        : m_buffer(capacity), m_capacity(capacity), m_size(0), m_head(0), m_tail(0), m_closed(false) {
        if (capacity == 0) {
            throw std::invalid_argument("Ring buffer capacity must be greater than 0");
        }
    }

    /**
     * Pushes an item into the buffer.
     * @param item The item to push
     * @param blocking If true, waits until space is available; if false, returns false when full
     * @return true if successful, false if buffer is full (in non-blocking mode) or closed
     */
    bool push(const T& item, bool blocking = true) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        if (m_closed) {
            return false;
        }
        
        if (m_size == m_capacity) {
            if (!blocking) {
                return false; // Buffer full and non-blocking
            }
            
            // Wait until space is available or buffer is closed
            m_notFull.wait(lock, [this] { 
                return m_size < m_capacity || m_closed; 
            });
            
            if (m_closed) {
                return false;
            }
        }
        
        // Add item to buffer at head position
        m_buffer[m_head] = item;
        m_head = (m_head + 1) % m_capacity;
        m_size++;
        
        // Notify consumers that data is available
        lock.unlock();
        m_notEmpty.notify_one();
        return true;
    }

    /**
     * Retrieves an item from the buffer.
     * @param item Reference to store the retrieved item
     * @param blocking If true, waits until an item is available; if false, returns false when empty
     * @return true if successful, false if buffer is empty (in non-blocking mode) or closed and empty
     */
    bool pop(T& item, bool blocking = true) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        if (m_size == 0) {
            if (!blocking || m_closed) {
                return false; // Buffer empty and non-blocking or closed
            }
            
            // Wait until data is available or buffer is closed
            m_notEmpty.wait(lock, [this] { 
                return m_size > 0 || m_closed; 
            });
            
            if (m_size == 0) {
                return false; // Buffer was closed while waiting
            }
        }
        
        // Get item from buffer at tail position
        item = m_buffer[m_tail];
        m_tail = (m_tail + 1) % m_capacity;
        m_size--;
        
        // Notify producers that space is available
        lock.unlock();
        m_notFull.notify_one();
        return true;
    }

    /**
     * Closes the buffer, preventing further pushes and waking up any waiting threads.
     */
    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    /**
     * Reopens a closed buffer.
     */
    void reopen() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = false;
    }

    /**
     * Returns the number of items currently in the buffer.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size;
    }

    /**
     * Returns the maximum capacity of the buffer.
     */
    size_t capacity() const {
        return m_capacity;
    }

    /**
     * Checks if the buffer is empty.
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size == 0;
    }

    /**
     * Checks if the buffer is full.
     */
    bool full() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size == m_capacity;
    }

    /**
     * Checks if the buffer is closed.
     */
    bool closed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    /**
     * Clears all items from the buffer.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_head = 0;
        m_tail = 0;
        m_size = 0;
        m_notFull.notify_all();
    }

private:
    std::vector<T> m_buffer;          // The actual buffer storage
    const size_t m_capacity;          // Maximum capacity
    size_t m_size;                    // Current number of items
    size_t m_head;                    // Write position
    size_t m_tail;                    // Read position
    bool m_closed;                    // Closed flag
    
    mutable std::mutex m_mutex;       // Mutex for thread safety
    std::condition_variable m_notEmpty; // Signaled when buffer becomes non-empty
    std::condition_variable m_notFull;  // Signaled when buffer becomes non-full
}; 