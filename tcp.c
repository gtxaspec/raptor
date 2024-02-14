#include <string.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>  // Include for the errno variable
#include <stdlib.h>

#include "encoder.h"
#include "system.h"
#include "tcp.h"

#define TAG "tcp"

#define SERVER_PORT 8080
#define BUFFER_SIZE 4096  // 4 KB

int setup_tcp() {
	// Setup TCP server
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

	while (1) {
		printf("Waiting for client connection...\n");
		client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
		if (client_socket == -1) {
			perror("Accept failed");
			continue;
		}
		printf("Client connected.\n");

		while (1) {
			int result = get_stream(client_socket, 0);
			if (result < 0) {
				perror("Error in get_stream");  // This will also display the error associated with errno
				printf("get_stream returned: %d, errno: %d\n", result, errno);
				break;
			}
		}

	if (client_socket != -1 && close(client_socket) == -1) {
		perror("Failed to close client socket");
	} else {
		client_socket = -1;  // Invalidate the socket descriptor after closing
	}
	printf("Client disconnected.\n");

	usleep(50000);  // Introduce a small delay (50ms) before accepting a new connection

	}

	close(server_socket);
	return 0;
}
