# Frame Processor

**Frame Processor** is a high-performance, multi-threaded C++ application designed for robust video ingestion, processing, and distribution. It ingests multiple RTSP streams, performs real-time frame transformations (cropping, scaling), and efficiently batches and publishes the results to Redis Streams for downstream analytics.

Designed for reliability and scalability, it employs a supervisor process pattern to ensure high availability and uses a modern C++20 codebase with efficient memory management.

## Key Features

*   **Multi-Stream Ingestion**: Concurrent handling of multiple RTSP video streams.
*   **Dynamic Reconfiguration**: Add, remove, or modify streams and processing rules at runtime without service interruption.
*   **High-Performance Processing**:
    *   Efficient thread pooling for parallel frame processing.
    *   FFmpeg-based filter graphs for high-quality cropping and scaling.
    *   Zero-copy optimizations where possible.
*   **Robust Architecture**:
    *   **Supervisor Pattern**: A parent process monitors the application and automatically restarts it in case of failures.
    *   **Graceful Shutdown**: Ensures all resources are released and queues flushed upon termination.
*   **Efficient Output**:
    *   Batches processed frames to reduce network overhead.
    *   Serializes data into a compact, padding-free binary format (YUV420P).
    *   Publishes to Redis Streams for decoupled consumption.
*   **Observability**: Structured logging (via `fmtlog`) and detailed error reporting.

## Architecture

The application follows a pipelined producer-consumer architecture:

1.  **Stream I/O Manager**: Connects to RTSP sources, demuxes, and decodes video frames. Pushes raw frames to the *Decode Queue*.
2.  **Worker Pool**: Consumes frames from the *Decode Queue*. It applies configured transformations (Crop/Scale) using FFmpeg filter graphs and pushes the results to the *Processed Queue*.
3.  **Publisher**: Consumes from the *Processed Queue*. It batches frames based on stream ID and event type, serializes them, and publishes the batches to Redis.

## Prerequisites

*   **Operating System**: Linux (Ubuntu 24.04 recommended)
*   **Compiler**: C++20 compatible compiler (Clang 18+ recommended)
*   **Build System**: CMake 3.16+
*   **Dependencies**:
    *   FFmpeg 8.0 (libavcodec, libavformat, libavutil, libswscale, libavfilter)
    *   fmt (11.0)
    *   simdjson (3.11)
    *   hiredis
    *   libcurl
    *   jemalloc (optional, recommended for performance)

## Installation

### Using Docker (Recommended)

The project includes a multi-stage Dockerfile that builds all dependencies from source to ensure version compatibility and optimization.

1.  **Build the image:**
    ```bash
    docker build -t frame-processor .
    ```

2.  **Run the container:**
    ```bash
    docker run -d \
      --net=host \
      -v $(pwd)/config:/etc/fp/configs \
      -e FP_REDIS_HOST=localhost \
      frame-processor
    ```

### Manual Build

1.  **Install system dependencies:**
    ```bash
    sudo apt-get update && sudo apt-get install -y \
        build-essential cmake git pkg-config \
        libcurl4-openssl-dev libhiredis-dev
    ```
    *Note: You may need to build FFmpeg 8.0, fmt 11, and simdjson from source if your distribution's repositories are outdated.*

2.  **Build the project:**
    ```bash
    mkdir build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release ..
    make -j$(nproc)
    ```

## Configuration

The application is configured via a JSON file and environment variables.

### Environment Variables

| Variable | Description | Default |
| :--- | :--- | :--- |
| `FP_REDIS_HOST` | **Required**. Hostname or IP of the Redis server. | - |
| `FP_REDIS_PORT` | Port of the Redis server. | `6379` |
| `FP_REDIS_QUEUE_BASE_NAME` | Base name for Redis Stream keys. | `fp_batches` |
| `FP_NUM_WORKER_THREADS` | Number of processing threads. | `CPU Cores - 2` |
| `FP_PUBLISHER_THREADS` | Number of concurrent Redis publisher threads. | `4` |

### Configuration File (`config.json`)

The configuration file defines the streams and their processing rules. It is typically located at `/etc/fp/configs/config.json`.

```json
{
  "global_settings": {
    "log_level": "INFO"
  },
  "streams": [
    {
      "id": 1,
      "url": "rtsp://user:pass@ip:port/stream",
      "enabled": true,
      "processing": {
        "crop": { "x": 0, "y": 0, "width": 1920, "height": 1080 },
        "scale": { "width": 640, "height": 360 }
      },
      "events": [
        {
          "name": "analytics_event_A",
          "target_fps": 5.0,
          "batch_size": 10
        }
      ]
    }
  ]
}
```

*   **processing**: Defines the crop region and target scale resolution.
*   **events**: Defines logical events associated with the stream.
    *   `batch_size`: Number of frames to accumulate before publishing to Redis.
    *   `target_fps`: (Logic handled by StreamIO) Target frame rate for this event.

## Output Format

Data is published to Redis Streams using the key pattern: `<FP_REDIS_QUEUE_BASE_NAME>:<stream_id>`.

Each entry contains:
*   **event**: The event name (e.g., "analytics_event_A").
*   **data**: A binary blob containing the batched frames.

### Binary Data Structure

The `data` blob consists of concatenated frames. Each frame in the batch has the following layout:

1.  **Header (16 bytes)**:
    ```cpp
    struct FrameHeader {
      int64_t  pts;     // Presentation Timestamp
      uint32_t width;   // Frame width
      uint32_t height;  // Frame height
    };
    ```
2.  **Y Plane**: `width * height` bytes.
3.  **U Plane**: `(width/2) * (height/2)` bytes.
4.  **V Plane**: `(width/2) * (height/2)` bytes.

*Note: The image data is tightly packed (no stride/padding) in YUV420P format.*



## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
