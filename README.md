# RTMP Relay

Small 1-to-1 RTMP relay built with standalone Asio and FFmpeg.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Usage

```bash
./build/rtmp_relay
```

Override default URLs with positional arguments:

```bash
./build/rtmp_relay rtmp://0.0.0.0:19350/live/in rtmp://127.0.0.1:19351/live/out
```

Fire up the player:
```bash
ffplay -fflags nobuffer -flags low_delay -listen 1 rtmp://0.0.0.0:19351/live/out
```

Then publish into it with something like (stream a pre-encoded file in a loop):

```bash
ffmpeg -re -stream_loop -1 -i sample.mp4 -c copy -f flv rtmp://127.0.0.1:19350/live/in
```

or (generate synthetic A/V on-the-fly):
```bash
ffmpeg -lavfi "testsrc=640x480,realtime;sine=10:40,arealtime" -f flv rtmp://127.0.0.1:19350/live/in
```

> `realtime`/`arealtime` are the `-lavfi` equivalent of `-re`, throttling synthetic sources to wall-clock speed.

## Docker

Build the image:

```bash
docker build -t rtmp-relay:ubuntu24.04 .
```

Run it with the default input/output URLs and expose the input port:

```bash
docker run --rm -it \
  --network host \
  rtmp-relay:ubuntu24.04
```

Or override the URLs:

```bash
docker run --rm -it \
  --network host \
  rtmp-relay:ubuntu24.04 \
  rtmp://0.0.0.0:19350/live/in rtmp://127.0.0.1:19351/live/out
```
