FROM mcr.microsoft.com/windows/servercore:ltsc2022

# Install Chocolatey
RUN powershell -NoProfile -InputFormat None -Command \
    "[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; \
    iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))" && \
    setx PATH "%PATH%;C:\ProgramData\chocolatey\bin" /M

# Install PowerShell 7
RUN choco install -y powershell-core --no-progress

# Install build tools and dependencies in smaller layers for clearer progress and easier debugging
RUN choco install -y git python nasm ninja --no-progress
RUN choco install -y mingw --no-progress
RUN choco install -y sccache --no-progress
RUN choco install -y imagemagick --no-progress

# Install CMake 4.2.0 via pip
RUN python -m pip install --upgrade pip && \
    python -m pip install "cmake==4.2.0"

# Set environment variables
ENV CC=gcc
ENV CXX=g++
ENV PATH="C:\Python314\Scripts;C:\ProgramData\mingw64\mingw64\bin;C:\ProgramData\chocolatey\bin;C:\Program Files\Git\cmd;C:\Program Files\ImageMagick-7.1.1-Q16-HDRI;C:\Program Files\PowerShell\7;${PATH}"

# Set working directory to the bind-mounted SPRING2 repo root
WORKDIR C:/spring2

# Display versions
RUN cmake --version && \
    gcc --version && \
    g++ --version && \
    ninja --version
