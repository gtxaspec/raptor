#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h> // For O_WRONLY and O_NONBLOCK
#include <sys/stat.h>

#include "encoder.h"
#include "system.h"
#include "tcp.h"
#include "ringbuffer.h"

#define TAG "tcp"

#define SERVER_PORT 8080
#define BUFFER_SIZE 131072
#define RING_BUFFER_SIZE 131072  // Must be a power of two

// Declare the ring buffer globally
ring_buffer_t videoRingBuffer;
char *ringBufferData;

// Path for the FIFO file
const char *fifoPath = "/tmp/h264_fifo";
// Function to create FIFO
void create_fifo() {
	if (mkfifo(fifoPath, 0666) == -1 && errno != EEXIST) {
		perror("Failed to create FIFO");
		exit(EXIT_FAILURE);
	}
}

int setup_tcp() {
	// Initialize the ring buffer
	ringBufferData = malloc(RING_BUFFER_SIZE);
	if (!ringBufferData) {
		perror("Failed to allocate ring buffer data");
		exit(1);
	}
	ring_buffer_init(&videoRingBuffer, ringBufferData, RING_BUFFER_SIZE);

	int server_socket, client_socket;
	struct sockaddr_in server_addr, client_addr;
	socklen_t addr_len = sizeof(client_addr);

	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket == -1) {
		perror("Socket creation failed");
		exit(1);
	}

	int opt_val = 1;
	setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &opt_val, sizeof(opt_val));
	setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

	int sendbuff = 2 * BUFFER_SIZE;
	setsockopt(server_socket, SOL_SOCKET, SO_SNDBUF, &sendbuff, sizeof(sendbuff));

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;

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

	create_fifo();

	while (1) {
		printf("Waiting for client connection...\n");
		client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
		if (client_socket == -1) {
			perror("Accept failed");
			continue; // Skip to the next iteration
		}
		printf("Client connected.\n");

		char buffer[BUFFER_SIZE]; // Temporary buffer for data to send

		while (1) {
			ring_buffer_size_t bytesAvailable = ring_buffer_num_items(&videoRingBuffer);
			if (bytesAvailable > 0) {
				ring_buffer_size_t bytesToRead = bytesAvailable < BUFFER_SIZE ? bytesAvailable : BUFFER_SIZE;
				ring_buffer_size_t bytesRead = ring_buffer_dequeue_arr(&videoRingBuffer, buffer, bytesToRead);
				if (bytesRead > 0) {
					int sentBytes = send(client_socket, buffer, bytesRead, 0);
					if (sentBytes < 0) {
						perror("Error sending data to client");
						break; // Break out of the loop on send error
					}
				}
			}
			usleep(100000);
		}

		if (client_socket != -1) {
			close(client_socket); // Close the client socket when done
			client_socket = -1;
		}
		printf("Client disconnected.\n");
	}

	close(server_socket);
	free(ringBufferData); // Free allocated memory for the ring buffer
	return 0;
}

void* video_feeder_thread(void *arg) {
	int video_source_channel = *((int*)arg); // Assuming video source channel is passed as an argument

	while (1) {
		feed_video_to_ring_buffer(&videoRingBuffer, video_source_channel);
		usleep(10000);  // Wait before trying to open FIFO again
	}

	return NULL;
}

void* fifo_writer_thread(void *arg) {
	const char *fifoPath = (const char *)arg;
	char buffer[BUFFER_SIZE];
	int fifo_fd = -1;

	while (1) {
		// Attempt to open FIFO if not already open
		if (fifo_fd == -1) {
			fifo_fd = open(fifoPath, O_WRONLY);
			if (fifo_fd == -1) {
				if (errno != ENXIO) {  // Ignore if no readers are currently available
					perror("Unable to open FIFO");
				}
				usleep(100000);  // Wait before trying to open FIFO again
				continue;
			}
		}

		// Check if there's data in the ring buffer
		ring_buffer_size_t bytesAvailable = ring_buffer_num_items(&videoRingBuffer);
		if (bytesAvailable > 0) {
			ring_buffer_size_t bytesToRead = bytesAvailable < BUFFER_SIZE ? bytesAvailable : BUFFER_SIZE;
			ring_buffer_size_t bytesRead = ring_buffer_dequeue_arr(&videoRingBuffer, buffer, bytesToRead);

			if (bytesRead > 0) {
				ssize_t bytesWritten = write(fifo_fd, buffer, bytesRead);
				if (bytesWritten == -1) {
					if (errno == EPIPE || errno == ENXIO) {
						// Reader has gone away, close and mark FIFO for reopening
						close(fifo_fd);
						fifo_fd = -1;
					} else {
						perror("Write to FIFO failed");
					}
				}
			}
		} else {
			// No data in the ring buffer, sleep briefly to yield CPU
			usleep(10000);
		}
	}

	if (fifo_fd != -1) {
		close(fifo_fd);
	}
	return NULL;
}


