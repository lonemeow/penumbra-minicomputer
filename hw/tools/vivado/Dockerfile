# syntax=docker/dockerfile:1
#
# Vivado for the Nexys Video flow. AMD publishes no Vivado image and its
# EULA forbids redistributing one, so this is built locally with your
# own AMD login and never pushed (`make vivado-image`). The built image
# is the archive of the working toolchain: rebuilding it needs AMD to
# still serve this release.
#
# The unpacked web installer arrives as the named build context
# `installer`, and the login token (`make vivado-authenticate`) as the
# build secret `amd_token`. Both are mounted into the one RUN that uses
# them, so neither becomes an image layer.

# `base` is also the environment the installer's own batch modes
# (ConfigGen, AuthTokenGen) run in, so they see the same system the
# install does.
FROM ubuntu:22.04 AS base

ENV DEBIAN_FRONTEND=noninteractive

# libtinfo5/libncurses5 are why this is 22.04: later Ubuntu releases
# dropped them. The X libraries serve the installer's bundled JRE. The
# base image has no UTF-8 locale, which Vivado expects.
RUN apt-get update && apt-get install -y --no-install-recommends \
        fontconfig \
        libfreetype6 \
        libncurses5 \
        libtinfo5 \
        libx11-6 \
        libxext6 \
        libxi6 \
        libxrender1 \
        libxtst6 \
        locales \
    && rm -rf /var/lib/apt/lists/* \
    && sed -i 's/^# *\(en_US.UTF-8 UTF-8\)/\1/' /etc/locale.gen \
    && locale-gen
ENV LANG=en_US.UTF-8

FROM base AS installed

# The one step with network access: the web installer downloads only
# the modules install_config.txt selects. It reads its token from
# ~/.Xilinx, so the secret is copied there for this RUN alone; the same
# RUN deletes it with the installer's other scratch, so none of it
# reaches the layer. `rw` so nothing the installer writes beside itself
# can fail the install; BuildKit discards those writes, and the host
# copy is never touched.
RUN --mount=type=bind,from=installer,target=/installer,rw \
    --mount=type=bind,source=install_config.txt,target=/config/install_config.txt \
    --mount=type=secret,id=amd_token,required=true \
    mkdir -p /root/.Xilinx \
    && install -m 0400 /run/secrets/amd_token /root/.Xilinx/wi_authentication_key \
    && /installer/xsetup --agree XilinxEULA,3rdPartyEULA \
        --batch Install --config /config/install_config.txt \
    && rm -rf /tmp/* /root/.Xilinx

# The install path embeds the release (<Destination>/<release>/Vivado),
# so a fixed alias keeps PATH release-independent.
RUN vivado_dir=$(dirname "$(find /tools/Xilinx -maxdepth 3 -path '*Vivado/settings64.sh')") \
    && test -x "$vivado_dir/bin/vivado" \
    && ln -s "$vivado_dir" /opt/vivado
ENV PATH=/opt/vivado/bin:$PATH

FROM installed

# Vivado's own runtime libraries. They sit after the install, not in
# `base`, so changing this list never invalidates the cached install
# (which would download it all again).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libpixman-1-0 \
    && rm -rf /var/lib/apt/lists/*

# Without this Vivado aborts in libudev (`realloc(): invalid pointer`)
# when its license library enumerates devices, which every
# route_design does. Vivado's bin/loader carries this same preload as
# its own Docker workaround, but only looks for RHEL's /lib64 path, and
# only when /.dockerenv exists, which BuildKit steps lack.
ENV LD_PRELOAD=/lib/x86_64-linux-gnu/libudev.so.1

# Prove the image can do its job before it is tagged: the pinned
# release, synthesizing and implementing a trivial design for our exact
# part. It runs as an unprivileged UID with no passwd entry and no
# network, the conditions the wrapper runs Vivado under.
ARG VIVADO_VERSION
USER 10000:10000
RUN --network=none \
    --mount=type=bind,source=smoke.tcl,target=/config/smoke.tcl \
    mkdir /tmp/smoke && cd /tmp/smoke \
    && HOME=/tmp/smoke vivado -mode batch -nojournal -log smoke.log \
        -source /config/smoke.tcl -tclargs "$VIVADO_VERSION" \
    && rm -rf /tmp/smoke

WORKDIR /work
