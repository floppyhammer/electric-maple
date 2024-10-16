# electric-maple

<!--
Copyright 2023, Pluto VR, Inc.
Copyright 2023-2024, Collabora, Ltd.

SPDX-License-Identifier: CC-BY-4.0
-->

## Description

Electric-maple is an open-source OpenXR-compatible XR Remote Rendering framework that provides a free and extensible alternative to remote XR streaming solutions like CloudXR and more local PCVR streaming solutions like Meta Quest Link. Its server counterpart builds and runs on Linux and Windows and its device counterpart includes OpenXR demo receiver applications for Linux and Android (Tested on Debian, Quest2 and Quest3). Built on top of [Monado](https://gitlab.freedesktop.org/monado/electric-maple-monado) for OpenXR compatibility and [GStreamer](https://github.com/GStreamer/gstreamer) for (hardware-accelerated) codecs and streaming/thetering, electric-Maple facilitates real-time delivery of XR applications from powerful servers to lightweight client devices such as AR/VR headsets and smart XR glasses.

## Structure

This repo has _two_ main projects:

* `server` - a regular desktop/server Linux CMake build. To do development on the server, open the `server` folder with your IDE of choice and build it like a regular CMake project. We're not doing anything crazy in that repo, should just work.

* `client` - an Android-only Gradle/CMake project. (It should take maybe a couple hours to port to desktop Linux.) To do development on the client, open the `client` folder with Android Studio and let it do its thing.

Also, both projects use the `monado` and `proto` folders (via CMake add_subdirectory) for general dependencies and message encoding/decoding.

## ADB port reversing

For now, we're going to assume everybody's debugging this with a Quest over ADB. The client and server are hard-coded (for now) to port `61943`, so if you run this:

```sh
adb reverse tcp:61943 tcp:61943
adb reverse tcp:8080 tcp:8080
```

your Quest should be able to connect to the server on your PC and you should be able to get going :)

## Native WebRTC test

Right now, we have a native WebRTC receiver test at `server/test/webrtc_client`. All it does is set up the stream and shove video into an autovideosink.

Not all of the state tracking is set up yet, so the only way it'll work is if you do things in this order:

* Launch `ems_streaming_server`
* Launch an OpenXR application (to trigger the GStreamer pipeline and webserver to start up)
* Launch the WebRTC client
