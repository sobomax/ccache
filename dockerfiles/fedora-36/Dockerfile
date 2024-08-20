ARG BASE_IMAGE=fedora:36
FROM ${BASE_IMAGE}

RUN dnf install -y \
        autoconf \
        bash \
        ccache \
        clang \
        cmake \
        diffutils \
        elfutils \
        findutils \
        gcc \
        gcc-c++ \
        hiredis-devel \
        libzstd-devel \
        make \
        python3 \
        redis \
 && dnf clean all
