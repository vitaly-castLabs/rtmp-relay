# RTMP Relay

1-to-1 RTMP relay built with standalone Asio and FFmpeg's AV libraries. Can be used for monitoring or alteration (adding / stripping metadata, re-encoding, encryption, etc).

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Usage

Default settings (listens on `rtmp://0.0.0.0:19350/live/in` and forwards to `rtmp://localhost:19351/live/out`):
```bash
./build/rtmp_relay
```
or 
```bash
./build/rtmp_relay rtmp://0.0.0.0:19350/live/in rtmp://rtmp.server.ip.here/live/stream
```

Full setup. Start the relay first:

```bash
./build/rtmp_relay
```

Then fire up the player:
```bash
ffplay -fflags nobuffer -flags low_delay -listen 1 rtmp://0.0.0.0:19351/live/out
```

Then publish into the relay with something like this (stream a pre-encoded file in a loop):

```bash
ffmpeg -re -stream_loop -1 -i sample.mp4 -c copy -f flv rtmp://127.0.0.1:19350/live/in
```

or (generate synthetic A/V on-the-fly):
```bash
ffmpeg -lavfi "testsrc=640x480,realtime;sine=10:40,arealtime" -f flv rtmp://127.0.0.1:19350/live/in
```

> `realtime`/`arealtime` are the `-lavfi` equivalent of `-re`, throttling synthetic sources to wall-clock speed.

## Transform plugins

The relay can load a shared library that transforms media frames on the fly.
A transform is created per stream; if the plugin returns NULL for a given
codec the stream is passed through unchanged.

```bash
./build/rtmp_relay --transform "dl=./build/transform.so;file=out.h264" \
    rtmp://0.0.0.0:19350/live/in rtmp://127.0.0.1:19351/live/out
```

The plugin must export four C functions declared in
[`include/transform_api.h`](include/transform_api.h):

```c
TransformContext* transform_create(const char* codec_name, const char* params);
void              transform_destroy(TransformContext* ctx);
size_t            transform_get_max_size(const TransformContext* ctx, size_t frame_size);
size_t            transform_apply(TransformContext* ctx,
                                  const uint8_t* src, size_t src_size,
                                  uint8_t* dst);
```

`codec_name` is the ffmpeg codec descriptor name (`"h264"`, `"aac"`, `"hevc"`,
`"av1"`, etc.). Frame data arrives in AVCC (length-prefixed) format for H.264/HEVC.

A sample plugin ([`samples/transform.c`](samples/transform.c)) is included. It handles H.264
only, dumps every frame to a file as Annex B, and appends a 16-byte filler NALU
to each frame before returning it to the relay.

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
