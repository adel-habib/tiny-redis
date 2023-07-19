package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"net"
)

func main() {
	// Define the flags
	var message = flag.String("message", "", "The message to send")
	var length = flag.Int("length", -1, "The length of the message. If not provided, the length is calculated.")

	// Parse the flags
	flag.Parse()

	// If no message is provided, print a usage message and exit
	if *message == "" {
		fmt.Println("You must provide a message with the -message flag.")
		return
	}

	// If no length is provided, calculate it from the message
	if *length == -1 {
		*length = len(*message)
	}

	// Connect to the server
	conn, err := net.Dial("tcp", "localhost:1234")
	if err != nil {
		fmt.Printf("Failed to connect to server: %v\n", err)
		return
	}
	defer conn.Close()

	// Prepare the header (4-byte length)
	header := make([]byte, 4)
	binary.LittleEndian.PutUint32(header, uint32(*length))

	// Send the header and the message
	_, err = conn.Write(append(header, []byte(*message)...))
	if err != nil {
		fmt.Printf("Failed to send message: %v\n", err)
		return
	}

	// Receive and print the response
	buf := make([]byte, 1024)
	n, err := conn.Read(buf)
	if err != nil {
		fmt.Printf("Failed to read response: %v\n", err)
		return
	}
	fmt.Printf("Received response: %s\n", string(buf[:n]))
}
