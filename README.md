# Raptor: Advanced IP Camera Streaming Platform

![Raptor Platform](https://github.com/gtxaspec/raptor/assets/12115272/bb36618c-549f-4d07-897a-cdb82e13b388)

Welcome to **Raptor**, an innovative and open-source platform designed for modular, multi-platform IP camera streaming. Our system is crafted to provide a versatile and robust solution for a wide range of streaming applications.

## Open-Source Licensing
Raptor is proudly released under the **GNU General Public License v3 (GPL-3)**. This license grants you the freedom to modify, distribute, and use the software for any purpose, as long as you adhere to the GPL-3 terms and conditions. This includes ensuring that any derivative work you distribute is under the same license, thus contributing to the open-source community's growth and innovation.

## Raptor Streaming System (RSS)
At the core of Raptor is the RSS, a suite of specialized daemons working in harmony to deliver an exceptional streaming experience.

### Raptor Video Daemon (RVD)
RVD is a powerhouse for video processing, offering a multitude of features:
- **Frame Source Integration**: Seamlessly fetches video from various sources.
- **Video Tuning**: Advanced video tuning for optimal visual quality.
- **Encoding and Synchronization**: Efficient encoding with integrated timestamp generation for perfect sync.
- **Unix Socket Exposure**: Encoded video streams are made available through abstract Unix sockets for versatile connectivity.

### Raptor Overlay Daemon (ROD)
ROD enhances your video streams with dynamic overlays:
- **Dynamic OSD**: Add on-screen display overlays to your video stream, enriching the viewer's experience.

### Raptor Audio Daemon (RAD)
RAD handles all aspects of audio streaming:
- **SDK Integration**: Utilizes SDK APIs in a modular fashion for seamless audio input/output operations.
- **Audio Sync**: Ensures perfect synchronization in the audio stream with timestamp generation.
- **Unix Socket Interface**: Audio streams are accessible via abstract Unix sockets, ensuring compatibility and ease of access.

### Raptor Motors Control (RMC) & Raptor IR Control (RIC)
Take control of your hardware with RMC and RIC:
- **IR Management**: Manage IRCUT and monitor exposure for precise IR control algorithms.
- **Hardware Integration**: Seamless integration with motor controls for camera adjustments.

### Raptor Media Recorder (RMR)
RMR is your solution for recording:
- **Versatile Recording**: Record audio and video to local or network storage with options for write-through or buffered writing.

### Raptor Media Daemon (RMD)
RMD combines and exports your streams:
- **Stream Merging**: Combine audio and video streams efficiently.
- **A/V Sync**: Ensure perfect synchronization between audio and video streams.
- **Network Streaming**: Ready for export to go2rtc or other network streaming solutions.

### Raptor V4L2 (RV4)
RV4 bridges your device with V4L2 applications through a compatibility layer, utilizing the [v4l2loopback](https://github.com/umlaeute/v4l2loopback) kernel module for seamless integration. This layer enhances multimedia projects by facilitating:
- **V4L2 Support**: Integrates with v4l2loopback module to extend V4L2 functionalities.
- **Stream Handling**: Manages video streams efficiently for use in V4L2 compliant applications.
- **Flexible Integration**: Offers compatibility for a broad range of multimedia projects with V4L2 requirements.

## Join the Raptor Community
We invite developers, streamers, and tech enthusiasts to contribute to the Raptor project. Whether it's by contributing code, providing feedback, or sharing your streaming experiences, your input is invaluable in making Raptor the most efficient and user-friendly streaming platform available.

Dive into the world of high-quality streaming with Raptor, on _your_ device – where every frame counts! 🚀📹
