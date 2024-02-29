#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#define SOCKET_PATH "/tmp/rvd_video"
#define V4L2LOOPBACK_DEVICE "/dev/video1"
#define BUFFER_SIZE 16384

int main(void) {
    printf("Raptor Video 4 Linux Daemon\n");
    int socket_fd, device_fd;
    struct sockaddr_un address;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read, bytes_written;

    // Open the Unix Domain Socket
    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCKET_PATH, sizeof(address.sun_path) - 1);

    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(struct sockaddr_un)) < 0) {
        perror("connect");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // Open the v4l2loopback device
    device_fd = open(V4L2LOOPBACK_DEVICE, O_WRONLY);
    if (device_fd < 0) {
        perror("open");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // Configure the device for H264 video format
    struct v4l2_format vid_format;
    memset(&vid_format, 0, sizeof(vid_format));
    vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    vid_format.fmt.pix.width = 1920;  // Example width, adjust as needed
    vid_format.fmt.pix.height = 1080; // Example height, adjust as needed
    vid_format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264; // Set format to H264
    vid_format.fmt.pix.sizeimage = 0;
    vid_format.fmt.pix.field = V4L2_FIELD_NONE;
    vid_format.fmt.pix.bytesperline = 0;
    vid_format.fmt.pix.colorspace = V4L2_PIX_FMT_YUV420;

    if (ioctl(device_fd, VIDIOC_S_FMT, &vid_format) < 0) {
        perror("ioctl");
        close(device_fd);
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // Main loop: read from socket, write to v4l2loopback device
    while ((bytes_read = read(socket_fd, buffer, BUFFER_SIZE)) > 0) {
        bytes_written = write(device_fd, buffer, bytes_read);
        if (bytes_written < 0) {
            perror("write");
            break;
        }
    }

    // Cleanup
    close(device_fd);
    close(socket_fd);

    return 0;
}
