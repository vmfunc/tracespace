# VHS plus what the demo needs: a compiler to build tracespace, and Neovim.
FROM ghcr.io/charmbracelet/vhs
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build git ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://github.com/neovim/neovim/releases/download/stable/nvim-linux-x86_64.tar.gz \
        | tar xz -C /opt \
    && ln -s /opt/nvim-linux-x86_64/bin/nvim /usr/local/bin/nvim
WORKDIR /src
ENTRYPOINT []
