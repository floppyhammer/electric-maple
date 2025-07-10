# Electric-Maple

<p align="center">
    <img src="logo/logo-based-on-noto-emoji.svg" alt="Electric-Maple logo">
</p>

<!--
Copyright 2023, Pluto VR, Inc.
Copyright 2023-2025, Collabora, Ltd.

SPDX-License-Identifier: CC-BY-4.0
-->

## Description

Electric-Maple is an open-source OpenXR-compatible XR Remote Rendering framework that provides a free and extensible alternative to remote XR streaming solutions like CloudXR and more local PCVR streaming solutions like Meta Quest Link. Its server counterpart builds and runs on Linux and Windows and its device counterpart includes OpenXR demo receiver applications for Linux and Android (Tested on Debian, Quest2 and Quest3). Built on top of [Monado](https://gitlab.freedesktop.org/monado/monado) for OpenXR compatibility and [GStreamer](https://github.com/GStreamer/gstreamer) for (hardware-accelerated) codecs and streaming/thetering, electric-Maple facilitates real-time delivery of XR applications from powerful servers to lightweight client devices such as AR/VR headsets and smart XR glasses.

## Structure

This repo has _two_ main projects:

* `server` - a regular desktop/server Linux CMake build. To do development on the server, open the `server` folder with your IDE of choice and build it like a regular CMake project. We're not doing anything crazy in that repo, should just work.

* `client` - an Android-only Gradle/CMake project. (It should take maybe a couple hours to port to desktop Linux.) To do development on the client, open the `client` folder with Android Studio and let it do its thing.

Also, both projects use the `monado` and `proto` folders (via CMake add_subdirectory) for general dependencies and message encoding/decoding.

## Connection Modes

The Electric-Maple server runs both a signaling server and a WebRTC server. The signaling server handles the initial connection and enables negotation of the WebRTC connection.

The client uses the websocket_uri to connect to the signaling server running on port 9240.

As part of the negotiation the server advertises its public IP addresses to the client using the signaling server connection.

The actual streaming then happens over the negotiated WebRTC connection.

### Network (wifi, ethernet)

Start the server (make sure to not use the `-l` argument).

```sh
./server/build/src/ems/ems_streaming_server
```

Set the debug.electric_maple.websocket_uri android property to `ws://<SERVER_IP>:9240/ws` using the ip address of the PC Electric-Maple server is running on. This is used to establish the connection to the signaling server.

```sh
adb shell setprop debug.electric_maple.websocket_uri "ws://192.168.1.2:9240/ws"
```

Then start the client through adb or the headset UI.

```sh
adb shell am start-activity -S -n org.freedesktop.electricmaple.standalone_client/org.freedesktop.electricmaple.standalone_client.StreamingActivity
```

Or use the client launch app script that compiles, installs & logs the client app.

```sh
./client/launch.sh
```

### ADB port reversing (USB)

Start the server with the `-l` or `--use-localhost` parameter. This will cause the signaling server to advertise localhost as viable WebRTC connection address.

```sh
./server/build/src/ems/ems_streaming_server -l
```

Run the `./client/setup-tunnel.sh` script to set the websocket_uri to localhost and reverse forward ports 9240 and 9241-9251.

```sh
./client/setup-tunnel.sh
```

Then start the client through adb or the headset UI.

```sh
adb shell am start-activity -S -n org.freedesktop.electricmaple.standalone_client/org.freedesktop.electricmaple.standalone_client.StreamingActivity
```


Or use the client launch app script with the `--use-localhost` parameter that compiles, installs & logs the client app.

With the `--use-localhost` parameter, the `setup-tunnel.sh` script will be run as part of the startup launch procedure.

```sh
./client/launch.sh --use-localhost
```

Your Quest should be able to connect to the server on your PC and you should be able to get going.

## Native WebRTC test

Right now, we have a native WebRTC receiver test at `server/test/webrtc_client`. All it does is set up the stream and shove video into an autovideosink.

Not all of the state tracking is set up yet, so the only way it'll work is if you do things in this order:

* Launch `ems_streaming_server`
* Launch an OpenXR application (to trigger the GStreamer pipeline and webserver to start up)
* Launch the WebRTC client
