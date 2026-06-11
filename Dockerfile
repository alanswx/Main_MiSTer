# Cross-compile the MiSTer Main binary for the DE10-Nano (ARMv7 hard-float Linux).
#
# The ARM GNU cross-toolchain has no macOS build, so we build inside Linux.
# On Apple Silicon this image runs natively (arm64) and uses the aarch64-hosted
# toolchain, so compiles are full speed with no x86 emulation.
#
# The toolchain is baked into the image (not the source tree), so rebuilds are
# just `make` against your mounted working copy.
#
# Build the image:  docker build -t mister-build .
# Compile:          docker run --rm -v "$PWD":/src mister-build make
# (or just use ./build-docker.sh)

FROM arm64v8/ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        wget xz-utils build-essential make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Download + extract ARM's aarch64-hosted arm-none-linux-gnueabihf toolchain
# into /opt via the repo's own setup script.
ENV MISTER_GCC_HOST_ARCH=aarch64
ENV MISTER_GCC_INSTALL_DIR=/opt/mister-toolchain
COPY setup_default_toolchain.sh /tmp/setup_default_toolchain.sh
# The setup script extracts into the current directory, so cd into the install
# dir first (it only uses MISTER_GCC_INSTALL_DIR for the PATH/existence check).
RUN mkdir -p "$MISTER_GCC_INSTALL_DIR" \
    && cd "$MISTER_GCC_INSTALL_DIR" \
    && bash -c 'source /tmp/setup_default_toolchain.sh'

ENV PATH="/opt/mister-toolchain/gcc-arm-10.2-2020.11-aarch64-arm-none-linux-gnueabihf/bin:${PATH}"

WORKDIR /src
CMD ["make"]
