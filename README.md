# RTMP Relay

Small 1-to-1 RTMP relay built with standalone Asio and FFmpeg.

It:

- opens one RTMP input
- opens one RTMP output
- can optionally listen for an incoming RTMP publisher on the input side
- remuxes streams 1-to-1 without transcoding
- logs every video packet/frame with size, timestamps, duration, and keyframe state
- stops cleanly on `SIGINT` or `SIGTERM`

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Usage

```bash
./build/rtmp_relay rtmp://127.0.0.1/live/in rtmp://127.0.0.1/live/out
```

To make the relay listen for a publisher first, then connect to the output only after a publisher is accepted:

```bash
./build/rtmp_relay --listen-input rtmp://0.0.0.0:19350/live/in rtmp://127.0.0.1:19351/live/out
```

Then publish into it with something like:

```bash
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://127.0.0.1:19350/live/in
```

Example log line:

```text
[2026-04-07 18:52:11.123] video frame #42 size=18341 pts=378000 (4.200s) dts=378000 (4.200s) duration=9000 (0.100s) key=no
```
