# syntax=docker/dockerfile:1
#
# openFPGALoader at a pinned upstream commit, for programming Xilinx
# boards: `make openfpgaloader-image`. Built from source because the
# release the OSS CAD Suite image ships cannot parse current Vivado
# bitstreams, and a separate image keeps the ULX3S toolchain unchanged.

FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        git \
        libftdi1-dev \
        libhidapi-dev \
        libudev-dev \
        make \
        pkg-config \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Only the package installs above run as root. Fetching by commit hash
# makes git verify the source against the pin; a GitHub tarball carries
# no such guarantee.
ARG OFL_COMMIT
USER 10000:10000
ENV HOME=/tmp/ofl
RUN mkdir -p /tmp/ofl/src && cd /tmp/ofl/src \
    && git init -q \
    && git fetch -q --depth 1 https://github.com/trabucayre/openFPGALoader.git "$OFL_COMMIT" \
    && git checkout -q FETCH_HEAD \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/tmp/ofl/install \
    && cmake --build build -j"$(nproc)" \
    && cmake --install build

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libftdi1-2 \
        libhidapi-libusb0 \
        libudev1 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

# The binary and its data directory: the latter holds the bridge
# bitstreams openFPGALoader loads to write a board's SPI flash.
COPY --from=build /tmp/ofl/install/ /usr/local/

# Fail the image build, not the first flash, if a runtime library is
# missing or the binary cannot start.
USER 10000:10000
RUN ! ldd /usr/local/bin/openFPGALoader | grep 'not found' \
    && openFPGALoader --Version

WORKDIR /work
