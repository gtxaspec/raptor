#!/bin/sh

# Configuration variables
CLIENT_IP="$1"
CLIENT_PORT="12345"
VIDEO_SOCKET="/tmp/rvd_video"
RAPTOR_BINARY="./raptor"

# Function to safely terminate processes
safe_kill() {
	killall -q -9 "$1" 2>/dev/null
	sleep 1 # Provide a moment for the process to terminate
}

if [ -z "$1" ]; then
	echo "Please provide an IP address.  The configured port is set to $CLIENT_PORT."
	exit 1
fi

# Check if socat is installed
if ! command -v socat >/dev/null 2>&1; then
	echo "Error: socat is not installed. Please install socat and try again."
	exit 1
fi

# Check if the second parameter is provided and non-empty
if [ -n "$2" ]; then
	SOCAT_BUFFER="-b $2"
fi

# Stop previous instances
safe_kill socat
safe_kill raptor

# Start the raptor process
$RAPTOR_BINARY &
RAPTOR_PID=$! # Store the process ID

# Wait for raptor to initialize
sleep 1

# Instructions for the user
echo -e "\n\nPlease run GStreamer on the client, now:"
echo -e "sudo chrt -f 99 gst-launch-1.0 -vvv udpsrc port=${CLIENT_PORT} ! h264parse ! avdec_h264 ! fpsdisplaysink sync=false\n\n"

# Forward video stream from UNIX socket to UDP endpoint
socat $SOCAT_BUFFER -d UDP-SENDTO:${CLIENT_IP}:${CLIENT_PORT},reuseaddr UNIX-CONNECT:${VIDEO_SOCKET}

# Cleanup: terminate the background processes
kill "$RAPTOR_PID"
safe_kill socat
