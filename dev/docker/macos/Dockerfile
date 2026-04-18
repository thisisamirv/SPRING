FROM ubuntu:latest

# macOS-equivalent build environment approximation
# Note: This uses Linux with equivalent tools since true macOS containers require special licensing
# For actual macOS testing, consider using GitHub Actions or a native macOS machine

RUN apt-get update && apt-get install -y \
    build-essential \
    ccache \
    clang \
    clang-tidy \
    cppcheck \
    libomp-dev \
    lld \
    mold \
    nasm \
    ninja-build \
    python3 \
    python3-pip \
    python3-venv \
    llvm \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install CMake 4.2.0 via pip in a Python virtualenv
RUN python3 -m venv /opt/cmake-venv && \
    /opt/cmake-venv/bin/python -m pip install --upgrade pip && \
    /opt/cmake-venv/bin/python -m pip install "cmake==4.2.0"

# Add CMake to PATH
ENV PATH="/opt/cmake-venv/bin:${PATH}"

# Set default C/C++ compilers to clang (matching macOS)
ENV CC=clang
ENV CXX=clang++

# Set working directory to the bind-mounted SPRING2 repo root
WORKDIR /spring2

# Display versions
RUN cmake --version && \
    clang --version && \
    clang++ --version && \
    ninja --version
