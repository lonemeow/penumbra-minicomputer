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
    unzip \
    && rm -rf /var/lib/apt/lists/*

# Install OSS CAD Suite (installs Yosys, nextpnr-ecp5, ujprog, etc.)
RUN curl -L https://github.com/YosysHQ/oss-cad-suite-build/releases/download/2026-04-08/oss-cad-suite-linux-x64-20260408.tgz | tar -xzC /opt

# sv2v: SystemVerilog to Verilog-2005 converter (pinned version)
# Yosys's read_verilog -sv doesn't support module-level import;
# sv2v converts full SV to plain Verilog that Yosys handles natively.
RUN curl -fsSL https://github.com/zachjs/sv2v/releases/download/v0.0.13/sv2v-Linux.zip \
        -o /tmp/sv2v.zip \
    && unzip -o /tmp/sv2v.zip -d /tmp \
    && mv /tmp/sv2v-Linux/sv2v /usr/local/bin/sv2v \
    && chmod +x /usr/local/bin/sv2v \
    && rm -rf /tmp/sv2v.zip /tmp/sv2v-Linux

# Toolchain environment
ENV PATH="/opt/oss-cad-suite/bin:${PATH}"
WORKDIR /work

CMD ["/bin/bash"]
