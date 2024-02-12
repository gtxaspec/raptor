#include <stdlib.h>
#include <imp/imp_log.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#include "sample-common.h"

#include <string.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>  // Include for the errno variable

#define SERVER_PORT 8080
#define BUFFER_SIZE 2048  // 4 KB

#define TAG "Sample-Encoder-video"

extern struct chn_conf chn[];

void displayUsage() {
	printf("usage: ingenic-vidcap [args...]\n\n"
		" --help            display this help message\n");
	exit(0);
}

int main(int argc, char *argv[])
{
	int i, ret;

	// parse args
	for (i = 0; i < argc; i++) {
		char *arg = argv[i];

		if (*arg == '-') {
			arg++;
			if (*arg == '-') arg++; // tolerate 2 dashes

			if (strcmp(arg, "help") == 0) {
				displayUsage();
				exit(0);
			} else {
				printf("unrecognized argument %s\n\n", argv[i]);
				displayUsage();
				exit(2);
			}
		}
	}

	/* Step.1 System init */
	ret = sample_system_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_System_Init() failed\n");
		return -1;
	}

	/* Step.2 FrameSource init */
	ret = sample_framesource_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource init failed\n");
		return -1;
	}

	/* Step.3 Encoder init */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_Encoder_CreateGroup(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_CreateGroup(%d) error !\n", chn[i].index);
				return -1;
			}
		}
	}

	ret = sample_encoder_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder init failed\n");
		return -1;
	}

	/* Step.4 Bind */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_Bind(&chn[i].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "Bind FrameSource channel%d and Encoder failed\n",i);
				return -1;
			}
		}
	}

	/* Step.5 Stream On */
	ret = sample_framesource_streamon();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "ImpStreamOn failed\n");
		return -1;
	}

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



/* Exit sequence as follow */

	/* Step.a Stream Off */
	ret = sample_framesource_streamoff();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource StreamOff failed\n");
		return -1;
	}

	/* Step.b UnBind */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_UnBind(&chn[i].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "UnBind FrameSource channel%d and Encoder failed\n",i);
				return -1;
			}
		}
	}

	/* Step.c Encoder exit */
	ret = sample_encoder_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder exit failed\n");
		return -1;
	}

	/* Step.d FrameSource exit */
	ret = sample_framesource_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource exit failed\n");
		return -1;
	}

	/* Step.e System exit */
	ret = sample_system_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "sample_system_exit() failed\n");
		return -1;
	}

	return 0;
}
