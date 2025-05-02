#include "webrtc_session.h"
#include <iostream>

// Simplified WebRTCSession implementation
WebRTCSession::WebRTCSession() : m_isConnected(false) {
    std::cout << "WebRTC Session created" << std::endl;
}

WebRTCSession::~WebRTCSession() {
    Stop();
    std::cout << "WebRTC Session destroyed" << std::endl;
}

bool WebRTCSession::Initialize(const std::string& signalingServerUrl) {
    std::cout << "Initializing WebRTC session with signaling server: " << signalingServerUrl << std::endl;
    m_signalingServerUrl = signalingServerUrl;
    m_isConnected = true;
    
    // Call callbacks for demonstration
    if (m_connectionStateCallback) {
        m_connectionStateCallback("connected");
    }
    
    if (m_iceStateCallback) {
        m_iceStateCallback("connected");
    }
    
    return true;
}

bool WebRTCSession::ConnectToPeer(const std::string& peerId) {
    std::cout << "Connecting to peer: " << peerId << std::endl;
    m_peerId = peerId;
    return true;
}

bool WebRTCSession::PushEncodedFrame(const uint8_t* data, size_t size, bool isKeyFrame) {
    // Just log the frame info without actual processing
    std::cout << "Received encoded frame: size=" << size << ", keyFrame=" << (isKeyFrame ? "true" : "false") << std::endl;
    return true;
}

void WebRTCSession::Stop() {
    std::cout << "Stopping WebRTC session" << std::endl;
    m_isConnected = false;
    
    if (m_connectionStateCallback) {
        m_connectionStateCallback("disconnected");
    }
} 