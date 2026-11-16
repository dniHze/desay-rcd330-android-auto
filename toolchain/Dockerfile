# Desay DRA7 / armhf (hard-float) cross toolchain + firmware repack/analysis tools.
# Target device: glibc 2.15, libstdc++ (gcc~4.8, GLIBCXX_3.4), kernel 3.14, ld-linux-armhf.so.3
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf binutils-arm-linux-gnueabihf \
      cpp-arm-linux-gnueabihf libc6-dev-armhf-cross \
      make cmake pkg-config file xxd bsdmainutils patchelf \
      gdb-multiarch \
      u-boot-tools device-tree-compiler srecord cpio gzip xz-utils \
      squashfs-tools e2fsprogs mtd-utils \
      binwalk \
      python3 python3-pip \
      ca-certificates git less vim-tiny \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["/bin/bash"]
