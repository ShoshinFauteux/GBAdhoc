# drprof image: mipsel cross compiler + qemu-mipsel WITH TCG plugins.
# Ubuntu's qemu-user is built without --enable-plugins, so qemu is built here.
#   docker build -t drprof-qemu tools/drprof/docker
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    gcc-mipsel-linux-gnu g++-mipsel-linux-gnu libc6-dev-mipsel-cross binutils-mipsel-linux-gnu \
    make python3 python3-venv gcc g++ libglib2.0-dev pkg-config ninja-build flex bison bzip2 \
    zlib1g-dev git ca-certificates wget xz-utils file rsync \
 && rm -rf /var/lib/apt/lists/*
RUN cd /opt && wget -q https://download.qemu.org/qemu-9.2.2.tar.xz && tar xf qemu-9.2.2.tar.xz \
 && rm qemu-9.2.2.tar.xz && cd qemu-9.2.2 \
 && ./configure --target-list=mipsel-linux-user --enable-plugins --disable-docs \
      --disable-werror --prefix=/opt/qemu \
 && make -j8 && make install && cp include/qemu/qemu-plugin.h /opt/qemu/include/ \
 && cd /opt && rm -rf qemu-9.2.2
ENV PATH=/opt/qemu/bin:$PATH
