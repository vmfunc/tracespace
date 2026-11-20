# Linux build-and-test environment: compiler, a terminal emulator library for
# driving the TUI through a pty, and a Neovim new enough for vim.uv (0.10+).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build git ca-certificates curl python3 python3-pyte \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://github.com/neovim/neovim/releases/download/stable/nvim-linux-x86_64.tar.gz \
        | tar xz -C /opt \
    && ln -s /opt/nvim-linux-x86_64/bin/nvim /usr/local/bin/nvim \
    && nvim --version | head -1
WORKDIR /src
