package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"sync"

	"github.com/gorilla/websocket"
)

// Client represents a connected client (browser or desktop app)
type Client struct {
	ID   string
	Conn *websocket.Conn
	Send chan []byte
}

// Message represents a signaling message
type Message struct {
	Type      string          `json:"type"`
	SrcID     string          `json:"srcId"`
	DestID    string          `json:"destId,omitempty"`
	Payload   json.RawMessage `json:"payload"`
	Timestamp int64           `json:"timestamp"`
}

// Global variables
var (
	clients    = make(map[string]*Client)
	clientsMux sync.Mutex
	upgrader   = websocket.Upgrader{
		CheckOrigin: func(r *http.Request) bool {
			return true // Allow all origins for now
		},
	}
)

// Handle WebSocket connections
func handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("WebSocket upgrade error: %v", err)
		return
	}

	clientID := r.URL.Query().Get("id")
	if clientID == "" {
		log.Println("Client connected without ID, generating random ID")
		clientID = fmt.Sprintf("client-%d", len(clients)+1)
	}

	// Create a new client
	client := &Client{
		ID:   clientID,
		Conn: conn,
		Send: make(chan []byte, 256),
	}

	// Register the client
	clientsMux.Lock()
	clients[clientID] = client
	clientsMux.Unlock()

	log.Printf("Client connected: %s", clientID)

	// Send client list to all clients
	broadcastClientList()

	// Start goroutines for reading and writing
	go readPump(client)
	go writePump(client)

	// Send a welcome message
	welcome := Message{
		Type:      "welcome",
		SrcID:     "server",
		DestID:    clientID,
		Timestamp: 0,
	}

	welcomePayload := struct {
		ID      string   `json:"id"`
		Message string   `json:"message"`
		Clients []string `json:"clients"`
	}{
		ID:      clientID,
		Message: "Welcome to the signaling server",
		Clients: getClientIDs(),
	}

	payloadBytes, _ := json.Marshal(welcomePayload)
	welcome.Payload = payloadBytes

	welcomeBytes, _ := json.Marshal(welcome)
	client.Send <- welcomeBytes
}

// Read messages from the client
func readPump(client *Client) {
	defer func() {
		// Unregister the client when the function returns
		clientsMux.Lock()
		delete(clients, client.ID)
		clientsMux.Unlock()

		client.Conn.Close()
		close(client.Send)

		log.Printf("Client disconnected: %s", client.ID)
		broadcastClientList()
	}()

	for {
		_, message, err := client.Conn.ReadMessage()
		if err != nil {
			if websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseAbnormalClosure) {
				log.Printf("WebSocket error: %v", err)
			}
			break
		}

		// Parse and route the message
		var msg Message
		if err := json.Unmarshal(message, &msg); err != nil {
			log.Printf("Error unmarshaling message: %v", err)
			continue
		}

		// Set the source ID to the client's ID
		msg.SrcID = client.ID

		// Route the message
		routeMessage(msg)
	}
}

// Write messages to the client
func writePump(client *Client) {
	defer client.Conn.Close()

	for {
		select {
		case message, ok := <-client.Send:
			if !ok {
				// The channel is closed
				client.Conn.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}

			if err := client.Conn.WriteMessage(websocket.TextMessage, message); err != nil {
				log.Printf("Error writing message: %v", err)
				return
			}
		}
	}
}

// Route a message to its destination
func routeMessage(msg Message) {
	// Marshal the message
	msgBytes, err := json.Marshal(msg)
	if err != nil {
		log.Printf("Error marshaling message: %v", err)
		return
	}

	if msg.DestID != "" {
		// Route to a specific client
		clientsMux.Lock()
		if client, ok := clients[msg.DestID]; ok {
			client.Send <- msgBytes
		} else {
			log.Printf("Client not found: %s", msg.DestID)
		}
		clientsMux.Unlock()
	} else {
		// Broadcast to all clients except the sender
		clientsMux.Lock()
		for id, client := range clients {
			if id != msg.SrcID {
				client.Send <- msgBytes
			}
		}
		clientsMux.Unlock()
	}
}

// Get a list of all client IDs
func getClientIDs() []string {
	clientsMux.Lock()
	defer clientsMux.Unlock()

	ids := make([]string, 0, len(clients))
	for id := range clients {
		ids = append(ids, id)
	}
	return ids
}

// Broadcast the client list to all clients
func broadcastClientList() {
	ids := getClientIDs()

	// Create a client list message
	clientList := Message{
		Type:      "client-list",
		SrcID:     "server",
		Timestamp: 0,
	}

	payload := struct {
		Clients []string `json:"clients"`
	}{
		Clients: ids,
	}

	payloadBytes, _ := json.Marshal(payload)
	clientList.Payload = payloadBytes

	// Marshal the message
	msgBytes, _ := json.Marshal(clientList)

	// Broadcast to all clients
	clientsMux.Lock()
	for _, client := range clients {
		client.Send <- msgBytes
	}
	clientsMux.Unlock()
}

func main() {
	// Parse command line flags
	port := flag.Int("port", 8080, "Port to listen on")
	flag.Parse()

	// Set up HTTP routes
	http.HandleFunc("/ws", handleWebSocket)
	http.Handle("/", http.FileServer(http.Dir("../web")))

	// Start the HTTP server
	addr := fmt.Sprintf(":%d", *port)
	log.Printf("Starting signaling server on %s", addr)
	log.Fatal(http.ListenAndServe(addr, nil))
}
