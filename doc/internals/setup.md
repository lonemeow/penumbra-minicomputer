# Penumbra Development Environment Setup

## Platform
Developed on Ubuntu 22.04 (WSL2). Instructions assume a Debian-based system.

## Required Tools

### Simulation (Docker-based — recommended)
| Tool | Purpose |
|------|---------|
| **Docker** | Runs Verilator inside a container (no host install needed) |
| **Make** | Build system |
| **GTKWave** | Waveform viewer (runs on host) |

The Makefile uses the official `verilator/verilator:latest` Docker image (currently Verilator 5.046). This avoids version issues with Ubuntu's outdated apt package (4.038).

### Synthesis & FPGA (install when ready to target ULX3S)
| Tool | Purpose |
|------|---------|
| **Yosys** | Synthesis (Verilog → netlist) |
| **nextpnr-ecp5** | Place-and-route for Lattice ECP5 |
| **Project Trellis** | ECP5 bitstream database |
| **openFPGALoader** | Program the ULX3S over USB |

For synthesis tools, the [OSS CAD Suite](https://github.com/YosysHQ/oss-cad-suite-build/releases) bundles everything in a single tarball.

## Install Steps

### 1. Docker (if not already installed)
Follow the [Docker Engine install for Ubuntu](https://docs.docker.com/engine/install/ubuntu/), then:
```bash
# Allow running docker without sudo
sudo usermod -aG docker $USER
# Log out and back in, then verify:
docker run hello-world
```

### 2. Pull the Verilator image
```bash
docker pull verilator/verilator:latest
```

### 3. GTKWave (for waveform viewing)
```bash
sudo apt install -y gtkwave
```

### 4. Run the smoke test
```bash
make smoke
```
This builds and runs a trivial adder module through Verilator (in Docker) to confirm the toolchain works. Expected output: `smoke_adder: 5/5 tests passed`.

## How It Works
The `Makefile` runs Verilator inside Docker, mounting the project directory as `/work`:
```
docker run --rm -v $(pwd):/work -w /work verilator/verilator:latest [verilator args...]
```
Built binaries are also executed inside the container (they link against the container's glibc).

## Alternative: Native Verilator Install
If you prefer to avoid Docker, install Verilator 5.x natively:

**Option A — OSS CAD Suite bundle:**
```bash
# Download latest from https://github.com/YosysHQ/oss-cad-suite-build/releases
tar -xzf oss-cad-suite-linux-x64-*.tgz -C ~/
echo 'export PATH="$HOME/oss-cad-suite/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

**Option B — Build from source:**
```bash
sudo apt install -y build-essential git autoconf flex bison help2man perl python3 ccache
git clone https://github.com/verilator/verilator.git
cd verilator && git checkout stable
autoconf && ./configure && make -j$(nproc) && sudo make install
```

Then override the Docker default in the Makefile or set `VERILATOR` directly.

## WSL2 Notes
- GTKWave requires WSLg (Windows 11 built-in) or an X server.
  Test with: `gtkwave &` — if a window appears, you're good.
- File I/O across the WSL/Windows boundary (`/mnt/c/...`) is slow.
  For faster builds, consider cloning the repo inside the Linux filesystem (`~/projects/penumbra-minicomputer`).
