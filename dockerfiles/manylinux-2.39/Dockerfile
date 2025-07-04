ARG BASE_IMAGE=quay.io/pypa/manylinux_2_39
FROM ${BASE_IMAGE} AS build

RUN dnf --refresh -y makecache
RUN dnf install -y dnf-plugins-core
RUN dnf config-manager --set-enabled crb
RUN dnf install -y \
        clang \
        cmake \
        diffutils \
        elfutils \
        gcc \
        gcc-c++ \
        less \
        libstdc++-devel \
        libstdc++-static \
        libzstd-devel \
        make \
        ninja-build \
        perl \
        python3
RUN dnf autoremove -y
RUN dnf clean all
