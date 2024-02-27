#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <pthread.h>

#include "encoder.h"
#include "system.h"
#include "ringbuffer.h"
#include "config.h"

#define TAG "uds"

#define SOCKET_PATH "/tmp/rvd_video" // Path to the Unix Domain Socket

// Declare the ring buffer globally
ring_buffer_t videoRingBuffer;
char *ringBufferData;

void signal_handler(int sig) {
	printf("Caught signal %d, cleaning up...\n", sig);
	
	// Remove the socket file
	unlink(SOCKET_PATH);
	
	exit(0);
}

void *handle_client(void *arg) {
	int client_socket = *((int*)arg);
	free(arg); // Free the allocated memory for the client socket

	char buffer[config.uds_buffer_size]; // Temporary buffer for data to send

	while (1) {
		ring_buffer_size_t bytesAvailable = ring_buffer_num_items(&videoRingBuffer);
		if (bytesAvailable > 0) {
			ring_buffer_size_t bytesToRead = bytesAvailable < config.uds_buffer_size ? bytesAvailable : config.uds_buffer_size;
			ring_buffer_size_t bytesRead = ring_buffer_dequeue_arr(&videoRingBuffer, buffer, bytesToRead);
			if (bytesRead > 0) {
				int sentBytes = send(client_socket, buffer, bytesRead, 0);
				if (sentBytes < 0) {
					perror("Error sending data to client");
					break; // Break out of the loop on send error
				}
			}
		}
		usleep(1000); // Adjust sleep as needed for CPU
	}

	close(client_socket); // Close the client socket when done
	printf("Client disconnected.\n");
	return NULL;
}

int setup_uds() {
	struct sigaction sa;
	// Set up the sigaction struct
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigfillset(&sa.sa_mask); // Block other signals while handling

	// Register the signal handlers for SIGINT and SIGTERM
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("Error setting signal handler for SIGINT");
		exit(EXIT_FAILURE);
	}
	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		perror("Error setting signal handler for SIGTERM");
		exit(EXIT_FAILURE);
	}
	// Initialize the ring buffer
	ringBufferData = malloc(config.ring_buffer_size);
	if (!ringBufferData) {
		perror("Failed to allocate ring buffer data");
		exit(1);
	}
	ring_buffer_init(&videoRingBuffer, ringBufferData, config.ring_buffer_size);

	int server_socket, client_socket;
	struct sockaddr_un server_addr, client_addr;
	socklen_t addr_len = sizeof(client_addr);

	// Use AF_UNIX for Unix Domain Socket
	server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_socket == -1) {
		perror("Socket creation failed");
		exit(1);
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sun_family = AF_UNIX;
	strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

	// Ensure the socket path does not exist before binding
	unlink(SOCKET_PATH);

	if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
		perror("Bind failed");
		close(server_socket);
		exit(1);
	}

	if (listen(server_socket, 1) == -1) {
		perror("Listen failed");
		close(server_socket);
		exit(1);
	}

	while (1) {
		printf("Waiting for client connection...\n");
		client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
		if (client_socket == -1) {
			perror("Accept failed");
			continue; // Skip to the next iteration
		}
		printf("Client connected.\n");

		// Allocate memory for client socket to pass to the thread
		int *client_sock_ptr = malloc(sizeof(int));
		*client_sock_ptr = client_socket;

		// Create a thread to handle the new client
		pthread_t client_thread;
		if (pthread_create(&client_thread, NULL, handle_client, client_sock_ptr) != 0) {
			perror("Failed to create thread");
			close(client_socket); // Close the client socket if thread creation fails
			free(client_sock_ptr); // Free the allocated memory in case of thread creation failure
			continue; // Skip to the next iteration
		}
		// Optional, detach the thread
		pthread_detach(client_thread);
	}

	close(server_socket);
	free(ringBufferData); // Free allocated memory for the ring buffer
	return 0;

}

void* video_feeder_thread(void *arg) {
	int video_source_channel = *((int*)arg); // Video source channel is passed as an argument

	while (1) {
		feed_video_to_ring_buffer(&videoRingBuffer, video_source_channel);
		usleep(1000);
	}

	return NULL;
}
