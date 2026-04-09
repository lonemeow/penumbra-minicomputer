FROM ubuntu:22.04

# Prevent interactive prompts during install
ENV DEBIAN_FRONTEND=noninteractive

# Install essential libraries for USB/FTDI and Python for scripts
RUN apt-get update && apt-get install -y \
    curl \
    python3 \
    python3-pip \
    libftdi1-2 \
    libusb-1.0-0 \
    picocom \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install OSS CAD Suite (installs Yosys, nextpnr-ecp5, ujprog, etc.)
RUN curl -L https://github.com/YosysHQ/oss-cad-suite-build/releases/download/2026-04-08/oss-cad-suite-linux-x64-20260408.tgz | tar -xzC /opt

# Toolchain environment
ENV PATH="/opt/oss-cad-suite/bin:${PATH}"
WORKDIR /work

CMD ["/bin/bash"]
