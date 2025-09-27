#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_BUFFER_SIZE 1024
#define MESSAGE_SIZE 256

// Message structure as defined in the assignment
typedef struct {
    int type;                // 1: Base64 message, 2: ACK, 3: Termination
    char content[MESSAGE_SIZE]; // Fixed size array for message content
} Message;

// Connection type enumerator
typedef enum {
    TCP_CONN,
    UDP_CONN
} ConnectionType;

// Global variables
int sock = -1;
struct sockaddr_in server_addr;
socklen_t addr_size;
ConnectionType conn_type;

// Base64 encoding and decoding functions
static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const unsigned char *data, size_t input_length) {
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(output_length + 1);
    if (encoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded_data[j++] = base64_chars[(triple >> 18) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 12) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 6) & 0x3F];
        encoded_data[j++] = base64_chars[triple & 0x3F];
    }

    // Add padding if necessary
    size_t mod_table[] = {0, 2, 1};
    for (size_t i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[output_length - 1 - i] = '=';

    encoded_data[output_length] = '\0';
    return encoded_data;
}

// Function to send a message
int send_message(const Message *message) {
    if (conn_type == TCP_CONN) {
        return send(sock, message, sizeof(Message), 0);
    } else { // UDP
        return sendto(sock, message, sizeof(Message), 0, 
                     (struct sockaddr*)&server_addr, addr_size);
    }
}

// Function to receive a message
int receive_message(Message *message) {
    if (conn_type == TCP_CONN) {
        return recv(sock, message, sizeof(Message), 0);
    } else { // UDP
        return recvfrom(sock, message, sizeof(Message), 0, 
                       (struct sockaddr*)&server_addr, &addr_size);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <tcp/udp>\n", argv[0]);
        return 1;
    }
    
    char *server_ip = argv[1];
    int port = atoi(argv[2]);
    
    // Determine connection type
    if (strcmp(argv[3], "tcp") == 0) {
        conn_type = TCP_CONN;
    } else if (strcmp(argv[3], "udp") == 0) {
        conn_type = UDP_CONN;
    } else {
        fprintf(stderr, "Invalid connection type. Use 'tcp' or 'udp'.\n");
        return 1;
    }
    
    // Initialize socket
    if (conn_type == TCP_CONN) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
    } else { // UDP
        sock = socket(AF_INET, SOCK_DGRAM, 0);
    }
    
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return 1;
    }
    
    addr_size = sizeof(server_addr);
    
    // Connect to server if using TCP
    if (conn_type == TCP_CONN) {
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Connection failed");
            close(sock);
            return 1;
        }
        printf("Connected to server at %s:%d using TCP\n", server_ip, port);
    } else {
        printf("Ready to communicate with server at %s:%d using UDP\n", server_ip, port);
    }
    
    char buffer[MAX_BUFFER_SIZE];
    Message message, response;
    int running = 1;
    
    while (running) {
        printf("\nEnter message (or 'quit' to exit): ");
        fgets(buffer, MAX_BUFFER_SIZE, stdin);
        
        // Remove newline character
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len-1] == '\n') {
            buffer[len-1] = '\0';
            len--;
        }
        
        if (strcmp(buffer, "quit") == 0) {
            // Send termination message
            message.type = 3; // Termination
            strcpy(message.content, "TERMINATE");
            send_message(&message);
            
            printf("Terminating connection...\n");
            running = 0;
            continue;
        }
        
        // Encode the message in Base64
        char *encoded = base64_encode((unsigned char*)buffer, len);
        if (!encoded) {
            printf("Failed to encode message\n");
            continue;
        }
        
        // Prepare and send the message
        message.type = 1; // Base64-encoded message
        strncpy(message.content, encoded, MESSAGE_SIZE - 1);
        message.content[MESSAGE_SIZE - 1] = '\0';
        
        printf("Sending Base64-encoded message: %s\n", encoded);
        free(encoded);
        
        if (send_message(&message) < 0) {
            perror("Failed to send message");
            continue;
        }
        
        // Wait for acknowledgment (only relevant for TCP)
        if (conn_type == TCP_CONN) {
            if (receive_message(&response) <= 0) {
                perror("Failed to receive response");
                running = 0;
                continue;
            }
            
            if (response.type == 2) { // ACK
                printf("Received acknowledgment: %s\n", response.content);
            } else {
                printf("Received unexpected response type: %d\n", response.type);
            }
        } else {
            // For UDP, we'll try to receive an acknowledgment with a timeout
            fd_set readfds;
            struct timeval tv;
            
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            
            // Set timeout to 2 seconds
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            
            int activity = select(sock + 1, &readfds, NULL, NULL, &tv);
            
            if (activity > 0 && FD_ISSET(sock, &readfds)) {
                if (receive_message(&response) > 0) {
                    if (response.type == 2) { // ACK
                        printf("Received acknowledgment: %s\n", response.content);
                    } else {
                        printf("Received unexpected response type: %d\n", response.type);
                    }
                }
            } else {
                printf("No acknowledgment received (expected with UDP)\n");
            }
        }
    }
    
    // Close the socket
    close(sock);
    return 0;
}