################################################################################
# Dockerfile: Ubuntu 24.04 LTS (Noble Numbat)
# - Multi-stage Production Build for Frame Processor
# - Enables all necessary repositories for multimedia libraries.
################################################################################

############################ STAGE : 1 - The Builder ############################
FROM ubuntu:24.04 AS builder

# 1) Set frontend to noninteractive and enable main, universe, restricted, multiverse repos
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common && \
    add-apt-repository main && \
    add-apt-repository universe && \
    add-apt-repository restricted && \
    add-apt-repository multiverse

# 2) Install all necessary build dependencies for Ubuntu 24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential g++ cmake git wget pkg-config libtool autoconf automake \
    ninja-build yasm nasm clang-18 curl ca-certificates dpkg-dev \
    lld libhiredis-dev libcurl4-openssl-dev \
    libx264-dev libx265-dev libvpx-dev libfdk-aac-dev libmp3lame-dev \
    libvorbis-dev libopus-dev libopenjp2-7-dev zlib1g-dev libssl-dev \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# 3) Set environment variables and alternatives to make clang the default compiler
RUN update-alternatives --install /usr/bin/cc cc /usr/bin/clang-18 100 && \
    update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-18 100
ENV CC=clang-18
ENV CXX=clang++-18

# 4) Build jemalloc from source
RUN git clone --depth 1 --branch 5.3.0 https://github.com/jemalloc/jemalloc.git /tmp/jemalloc && \
    cd /tmp/jemalloc && \
    ./autogen.sh && \
    ./configure CC=${CC} CXX=${CXX} --prefix=/usr/local && \
    make -j$(nproc) && make install && \
    ldconfig && rm -rf /tmp/jemalloc

# 5) Build simdjson from source
RUN git clone --depth 1 --branch v3.11.6 https://github.com/simdjson/simdjson.git /tmp/simdjson && \
    cd /tmp/simdjson && cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) && cmake --install build && rm -rf /tmp/simdjson

# 6) Build fmt from source
RUN git clone --depth 1 --branch 11.0.0 https://github.com/fmtlib/fmt.git /tmp/fmt && \
    cd /tmp/fmt && cmake -B build -DCMAKE_BUILD_TYPE=Release -DFMT_INSTALL=ON && \
    cmake --build build -j$(nproc) && cmake --install build && rm -rf /tmp/fmt

# 7) Build fmtlog from source
RUN git clone https://github.com/MengRao/fmtlog.git /tmp/fmtlog && \
    cd /tmp/fmtlog && git submodule update --init --recursive && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) && cmake --install build && rm -rf /tmp/fmtlog

# 8) Build FFmpeg from source
RUN wget -q https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz -O /tmp/ffmpeg.tar.xz && \
    tar -xJf /tmp/ffmpeg.tar.xz -C /tmp && cd /tmp/ffmpeg-8.0 && \
    ./configure \
        --cc=${CC} --cxx=${CXX} --prefix=/usr/local --enable-gpl --enable-nonfree \
        --enable-libx264 --enable-libx265 --enable-libvpx --enable-libfdk-aac --enable-libmp3lame \
        --enable-libvorbis --enable-libopus --enable-libopenjpeg \
        --disable-doc --disable-htmlpages --disable-manpages --disable-podpages && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/ffmpeg* && ldconfig

# 9) Copy and compile the Frame Processor project
WORKDIR /src
COPY . .

RUN cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-18 \
      -DCMAKE_CXX_COMPILER=clang++-18 && \
    cmake --build build -j$(nproc)

############################ STAGE : 2 - The Final Image ############################
FROM ubuntu:24.04

# 1) Enable multiverse for runtime libs and install dependencies
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common && \
    add-apt-repository multiverse && \
    apt-get update && apt-get install -y --no-install-recommends \
    libatomic1 libcurl4 libvpx9 libx264-164 libx265-199 libfdk-aac2 libhiredis1.1.0 \
    libmp3lame0 libvorbis0a libvorbisenc2 libopus0 libopenjp2-7 ca-certificates \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# 2) Copy necessary assets from the builder stage
COPY --from=builder /usr/local/lib/ /usr/local/lib/
COPY --from=builder /src/build/frame_processor /usr/local/bin/frame_processor

# 3) Create a directory for the configuration file with env & copy it
RUN mkdir -p /etc/fp/configs
COPY config/config.json /etc/fp/configs/config.json
COPY config/fp.env /etc/fp/configs/fp.env

# 4) Set up the runtime environment
RUN ldconfig
ENV LD_PRELOAD=/usr/local/lib/libjemalloc.so
WORKDIR /

# 5) Define the default command
CMD ["/bin/bash", "-c", "/usr/local/bin/frame_processor"]
