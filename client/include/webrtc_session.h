#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <functional>

// WebRTC peer session class
class WebRTCSession {
public:
    WebRTCSession();
    ~WebRTCSession();

    // Initialize the WebRTC session
    bool Initialize(const std::string& signalingServerUrl);
    
    // Connect to a peer
    bool ConnectToPeer(const std::string& peerId);
    
    // Push encoded H.264 frame data to WebRTC
    bool PushEncodedFrame(const uint8_t* data, size_t size, bool isKeyFrame);
    
    // Stop the session
    void Stop();
    
    // Callback types
    using IceStateCallback = std::function<void(const std::string& state)>;
    using ConnectionStateCallback = std::function<void(const std::string& state)>;
    
    // Set callbacks
    void SetIceStateCallback(IceStateCallback callback) { m_iceStateCallback = callback; }
    void SetConnectionStateCallback(ConnectionStateCallback callback) { m_connectionStateCallback = callback; }

private:
    // Signaling server connection
    std::string m_signalingServerUrl;
    bool m_isConnected = false;
    std::string m_peerId;
    
    // Callbacks
    IceStateCallback m_iceStateCallback;
    ConnectionStateCallback m_connectionStateCallback;
}; 