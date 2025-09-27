#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_CLIENTS 10
#define MAX_BUFFER_SIZE 1024
#define MESSAGE_SIZE 256

// Message structure as defined in the assignment
typedef struct {
    int type;                // 1: Base64 message, 2: ACK, 3: Termination
    char content[MESSAGE_SIZE]; // Fixed size array for message content
} Message;

// Client structure for TCP connections
typedef struct {
    int socket;
    struct sockaddr_in address;
    pthread_t thread_id;
} client_t;

// Global variables
int tcp_socket, udp_socket;
int keep_running = 1;
client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static int get_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1; // For padding or invalid characters
}

unsigned char *base64_decode(const char *data, size_t input_length, size_t *output_length) {
    if (input_length % 4 != 0) return NULL;

    *output_length = input_length / 4 * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;

    unsigned char *decoded_data = malloc(*output_length);
    if (decoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        int index_a = get_index(data[i++]);
        int index_b = get_index(data[i++]);
        int index_c = get_index(data[i++]);
        int index_d = get_index(data[i++]);

        if (index_a == -1 || index_b == -1) {
            free(decoded_data);
            return NULL;
        }

        uint32_t triple = (index_a << 18) + (index_b << 12);
        if (index_c != -1) triple += (index_c << 6);
        if (index_d != -1) triple += index_d;

        if (j < *output_length) decoded_data[j++] = (triple >> 16) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = triple & 0xFF;
    }

    return decoded_data;
}

// Add client to the array
void add_client(client_t *client) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == NULL) {
            clients[i] = client;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Remove client from the array
void remove_client(client_t *client) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == client) {
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Handle messages from a TCP client
void *handle_tcp_client(void *arg) {
    client_t *client = (client_t *)arg;
    Message message, response;
    int bytes_received;

    while ((bytes_received = recv(client->socket, &message, sizeof(Message), 0)) > 0) {
        printf("Received message from client - Type: %d\n", message.type);

        if (message.type == 1) { // Base64-encoded message
            size_t decoded_length;
            unsigned char *decoded = base64_decode(message.content, strlen(message.content), &decoded_length);
            
            if (decoded) {
                printf("Decoded message: %.*s\n", (int)decoded_length, decoded);
                
                // Send acknowledgment
                response.type = 2; // ACK
                strcpy(response.content, "Message received successfully");
                send(client->socket, &response, sizeof(Message), 0);
                
                free(decoded);
            } else {
                printf("Failed to decode message\n");
            }
        } else if (message.type == 3) { // Termination message
            printf("Client requested termination\n");
            break;
        }
    }

    // Close the socket and clean up
    close(client->socket);
    remove_client(client);
    free(client);
    pthread_detach(pthread_self());
    return NULL;
}

// Handle UDP messages
void handle_udp_message(struct sockaddr_in *client_addr) {
    Message message, response;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    
    // Receive message
    int bytes_received = recvfrom(udp_socket, &message, sizeof(Message), 0, 
                                 (struct sockaddr*)client_addr, &addr_size);
    
    if (bytes_received > 0) {
        printf("Received UDP message - Type: %d\n", message.type);
        
        if (message.type == 1) { // Base64-encoded message
            size_t decoded_length;
            unsigned char *decoded = base64_decode(message.content, strlen(message.content), &decoded_length);
            
            if (decoded) {
                printf("Decoded UDP message: %.*s\n", (int)decoded_length, decoded);
                
                // Send acknowledgment
                response.type = 2; // ACK
                strcpy(response.content, "Message received successfully");
                sendto(udp_socket, &response, sizeof(Message), 0, 
                      (struct sockaddr*)client_addr, addr_size);
                
                free(decoded);
            } else {
                printf("Failed to decode UDP message\n");
            }
        } else if (message.type == 3) { // Termination message
            printf("UDP client sent termination message\n");
        }
    }
}

// Signal handler
void handle_signal(int sig) {
    keep_running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    // Set up signal handler
    signal(SIGINT, handle_signal);

    // Initialize TCP socket
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("TCP socket creation failed");
        return 1;
    }

    // Set TCP socket options
    int opt = 1;
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt for TCP socket failed");
        return 1;
    }

    // Initialize UDP socket
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        perror("UDP socket creation failed");
        close(tcp_socket);
        return 1;
    }

    // Set UDP socket options
    if (setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt for UDP socket failed");
        close(tcp_socket);
        close(udp_socket);
        return 1;
    }

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // Bind TCP socket
    if (bind(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP binding failed");
        close(tcp_socket);
        close(udp_socket);
        return 1;
    }

    // Bind UDP socket
    if (bind(udp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("UDP binding failed");
        close(tcp_socket);
        close(udp_socket);
        return 1;
    }

    // Listen for TCP connections
    if (listen(tcp_socket, 10) < 0) {
        perror("TCP listen failed");
        close(tcp_socket);
        close(udp_socket);
        return 1;
    }

    printf("Server started on port %d\n", port);
    printf("Listening for TCP and UDP connections...\n");

    // Set up file descriptors for select()
    fd_set read_fds;
    int max_fd;

    // Main server loop
    while (keep_running) {
        FD_ZERO(&read_fds);
        FD_SET(tcp_socket, &read_fds);
        FD_SET(udp_socket, &read_fds);
        max_fd = (tcp_socket > udp_socket) ? tcp_socket : udp_socket;

        // Wait for activity on one of the sockets
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            if (keep_running == 0) break;
            perror("Select error");
            continue;
        }

        // Handle TCP connections
        if (FD_ISSET(tcp_socket, &read_fds)) {
            client_t *client = malloc(sizeof(client_t));
            if (!client) {
                perror("Failed to allocate memory for client");
                continue;
            }

            client->socket = accept(tcp_socket, (struct sockaddr*)&client->address, &addr_size);
            if (client->socket < 0) {
                perror("Accept failed");
                free(client);
                continue;
            }

            printf("New TCP connection from %s:%d\n", 
                  inet_ntoa(client->address.sin_addr), ntohs(client->address.sin_port));

            // Create a new thread for the client
            if (pthread_create(&client->thread_id, NULL, handle_tcp_client, (void*)client) != 0) {
                perror("Failed to create thread");
                close(client->socket);
                free(client);
                continue;
            }

            // Add client to the list
            add_client(client);
        }

        // Handle UDP messages
        if (FD_ISSET(udp_socket, &read_fds)) {
            handle_udp_message(&client_addr);
        }
    }

    // Clean up
    printf("Shutting down server...\n");
    
    // Close all client connections
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            close(clients[i]->socket);
            clients[i] = NULL;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    close(tcp_socket);
    close(udp_socket);
    return 0;
}