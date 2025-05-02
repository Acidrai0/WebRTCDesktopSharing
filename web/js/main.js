// DOM elements
const connectButton = document.getElementById('connect-button');
const disconnectButton = document.getElementById('disconnect-button');
const connectionStatus = document.getElementById('connection-status');
const connectionText = document.getElementById('connection-text');
const clientList = document.getElementById('client-list');
const peerIdText = document.getElementById('peer-id');
const remoteVideo = document.getElementById('remote-video');
const noStreamMessage = document.getElementById('no-stream-message');
const fullscreenButton = document.getElementById('fullscreen-button');
const qualitySelector = document.getElementById('quality-selector');

// WebRTC configuration
const iceServers = [
    { urls: 'stun:stun.l.google.com:19302' },
    { urls: 'stun:stun1.l.google.com:19302' },
    { urls: 'stun:stun2.l.google.com:19302' },
];

const peerConnectionConfig = {
    iceServers: iceServers,
    iceCandidatePoolSize: 10,
};

// Global state
let websocket = null;
let peerConnection = null;
let clientId = 'browser-' + Math.floor(Math.random() * 10000);
let selectedPeerId = null;
let isConnected = false;
let currentStream = null;

// Initialize the application
function init() {
    connectButton.addEventListener('click', connectToServer);
    disconnectButton.addEventListener('click', disconnectFromServer);
    fullscreenButton.addEventListener('click', toggleFullscreen);
    qualitySelector.addEventListener('change', changeQuality);
    
    // Handle video element events
    remoteVideo.addEventListener('loadedmetadata', () => {
        console.log('Remote video metadata loaded');
        noStreamMessage.classList.add('hidden');
        fullscreenButton.disabled = false;
        qualitySelector.disabled = false;
    });
    
    remoteVideo.addEventListener('ended', () => {
        console.log('Remote video ended');
        noStreamMessage.classList.remove('hidden');
        fullscreenButton.disabled = true;
        qualitySelector.disabled = true;
    });
}

// Connect to the signaling server
function connectToServer() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = window.location.hostname || 'localhost';
    const port = window.location.port || '8080';
    const wsUrl = `${protocol}//${host}:${port}/ws?id=${clientId}`;
    
    console.log(`Connecting to signaling server at ${wsUrl}`);
    
    websocket = new WebSocket(wsUrl);
    
    websocket.onopen = () => {
        console.log('Connected to signaling server');
        connectionStatus.textContent = 'Connected to Server';
        connectionStatus.classList.add('connected');
        connectionText.textContent = 'Connected to server';
        connectButton.disabled = true;
        disconnectButton.disabled = false;
        isConnected = true;
    };
    
    websocket.onclose = (event) => {
        console.log(`WebSocket closed: ${event.code} ${event.reason}`);
        disconnectFromServer();
    };
    
    websocket.onerror = (error) => {
        console.error('WebSocket error:', error);
        disconnectFromServer();
    };
    
    websocket.onmessage = handleSignalingMessage;
}

// Disconnect from the signaling server
function disconnectFromServer() {
    closeWebRTCConnection();
    
    if (websocket) {
        websocket.close();
        websocket = null;
    }
    
    connectionStatus.textContent = 'Disconnected';
    connectionStatus.classList.remove('connected');
    connectionText.textContent = 'Not connected';
    connectButton.disabled = false;
    disconnectButton.disabled = true;
    
    selectedPeerId = null;
    peerIdText.textContent = 'None';
    
    updateClientList([]);
    isConnected = false;
}

// Handle incoming signaling messages
function handleSignalingMessage(event) {
    try {
        const message = JSON.parse(event.data);
        
        console.log('Received message:', message.type);
        
        switch (message.type) {
            case 'welcome':
                handleWelcomeMessage(message);
                break;
            case 'client-list':
                handleClientListMessage(message);
                break;
            case 'offer':
                handleOfferMessage(message);
                break;
            case 'answer':
                handleAnswerMessage(message);
                break;
            case 'ice-candidate':
                handleIceCandidateMessage(message);
                break;
            default:
                console.warn('Unknown message type:', message.type);
        }
    } catch (error) {
        console.error('Error parsing message:', error);
    }
}

// Handle welcome message from the server
function handleWelcomeMessage(message) {
    const payload = JSON.parse(message.payload);
    console.log('Welcome message:', payload);
    clientId = payload.id;
    updateClientList(payload.clients);
}

// Handle client list update
function handleClientListMessage(message) {
    const payload = JSON.parse(message.payload);
    updateClientList(payload.clients);
}

// Handle WebRTC offer
function handleOfferMessage(message) {
    console.log('Received offer from:', message.srcId);
    
    // Set the selected peer ID
    selectedPeerId = message.srcId;
    peerIdText.textContent = selectedPeerId;
    
    // Create peer connection if it doesn't exist
    createPeerConnectionIfNeeded();
    
    const payload = JSON.parse(message.payload);
    const offer = payload.sdp;
    
    peerConnection.setRemoteDescription(new RTCSessionDescription(offer))
        .then(() => {
            console.log('Remote description set');
            return peerConnection.createAnswer();
        })
        .then(answer => {
            console.log('Created answer');
            return peerConnection.setLocalDescription(answer);
        })
        .then(() => {
            console.log('Local description set');
            sendSignalingMessage('answer', selectedPeerId, {
                sdp: peerConnection.localDescription
            });
        })
        .catch(error => {
            console.error('Error handling offer:', error);
        });
}

// Handle WebRTC answer
function handleAnswerMessage(message) {
    console.log('Received answer from:', message.srcId);
    
    if (!peerConnection) {
        console.warn('Received answer but no peer connection exists');
        return;
    }
    
    const payload = JSON.parse(message.payload);
    const answer = payload.sdp;
    
    peerConnection.setRemoteDescription(new RTCSessionDescription(answer))
        .then(() => {
            console.log('Remote description set from answer');
        })
        .catch(error => {
            console.error('Error handling answer:', error);
        });
}

// Handle ICE candidate
function handleIceCandidateMessage(message) {
    console.log('Received ICE candidate from:', message.srcId);
    
    if (!peerConnection) {
        console.warn('Received ICE candidate but no peer connection exists');
        return;
    }
    
    const payload = JSON.parse(message.payload);
    const candidate = payload.candidate;
    
    if (candidate) {
        peerConnection.addIceCandidate(new RTCIceCandidate(candidate))
            .catch(error => {
                console.error('Error adding ICE candidate:', error);
            });
    }
}

// Create WebRTC peer connection
function createPeerConnectionIfNeeded() {
    if (peerConnection) {
        console.log('Peer connection already exists');
        return;
    }
    
    console.log('Creating peer connection');
    
    peerConnection = new RTCPeerConnection(peerConnectionConfig);
    
    peerConnection.onicecandidate = event => {
        if (event.candidate) {
            console.log('Generated ICE candidate');
            sendSignalingMessage('ice-candidate', selectedPeerId, {
                candidate: event.candidate
            });
        }
    };
    
    peerConnection.oniceconnectionstatechange = () => {
        console.log('ICE connection state:', peerConnection.iceConnectionState);
    };
    
    peerConnection.ontrack = event => {
        console.log('Received remote track');
        currentStream = event.streams[0];
        remoteVideo.srcObject = currentStream;
    };
}

// Close WebRTC connection
function closeWebRTCConnection() {
    if (remoteVideo.srcObject) {
        const tracks = remoteVideo.srcObject.getTracks();
        tracks.forEach(track => track.stop());
        remoteVideo.srcObject = null;
    }
    
    if (peerConnection) {
        peerConnection.close();
        peerConnection = null;
    }
    
    noStreamMessage.classList.remove('hidden');
    fullscreenButton.disabled = true;
    qualitySelector.disabled = true;
}

// Update the client list in the UI
function updateClientList(clients) {
    // Filter out our own client ID
    const filteredClients = clients.filter(id => id !== clientId);
    
    // Clear the current list
    clientList.innerHTML = '';
    
    if (filteredClients.length === 0) {
        const noClientsItem = document.createElement('li');
        noClientsItem.textContent = 'No clients available';
        noClientsItem.classList.add('no-clients');
        clientList.appendChild(noClientsItem);
    } else {
        filteredClients.forEach(id => {
            const listItem = document.createElement('li');
            listItem.textContent = id;
            listItem.dataset.id = id;
            
            if (id === selectedPeerId) {
                listItem.classList.add('selected');
            }
            
            listItem.addEventListener('click', () => {
                connectToPeer(id);
            });
            
            clientList.appendChild(listItem);
        });
    }
}

// Connect to a peer
function connectToPeer(peerId) {
    // If we're already connected to this peer, do nothing
    if (selectedPeerId === peerId && peerConnection) {
        console.log('Already connected to this peer');
        return;
    }
    
    // Close any existing connection
    closeWebRTCConnection();
    
    // Set the selected peer
    selectedPeerId = peerId;
    peerIdText.textContent = peerId;
    
    // Update selection in the UI
    const items = clientList.querySelectorAll('li');
    items.forEach(item => {
        if (item.dataset.id === peerId) {
            item.classList.add('selected');
        } else {
            item.classList.remove('selected');
        }
    });
    
    // Create a new peer connection
    createPeerConnectionIfNeeded();
    
    // Create and send an offer
    peerConnection.createOffer()
        .then(offer => {
            console.log('Created offer');
            return peerConnection.setLocalDescription(offer);
        })
        .then(() => {
            console.log('Local description set');
            sendSignalingMessage('offer', selectedPeerId, {
                sdp: peerConnection.localDescription
            });
        })
        .catch(error => {
            console.error('Error creating offer:', error);
        });
}

// Send a signaling message
function sendSignalingMessage(type, destId, payload) {
    if (!websocket || websocket.readyState !== WebSocket.OPEN) {
        console.error('WebSocket not connected');
        return;
    }
    
    const message = {
        type: type,
        destId: destId,
        payload: payload,
        timestamp: Date.now()
    };
    
    websocket.send(JSON.stringify(message));
}

// Toggle fullscreen mode
function toggleFullscreen() {
    if (!document.fullscreenElement) {
        remoteVideo.requestFullscreen().catch(err => {
            console.error('Error attempting to enable fullscreen:', err);
        });
    } else {
        document.exitFullscreen();
    }
}

// Change video quality
function changeQuality() {
    const quality = qualitySelector.value;
    console.log('Changing quality to:', quality);
    
    // Send quality change request to peer
    if (selectedPeerId) {
        sendSignalingMessage('quality-change', selectedPeerId, {
            quality: quality
        });
    }
}

// Initialize the application when the DOM is loaded
document.addEventListener('DOMContentLoaded', init); 