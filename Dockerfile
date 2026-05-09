FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install core build tools and ncurses. Use --no-install-recommends to keep image small.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    make \
    libncurses-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Build all binaries during image creation
RUN make

CMD ["bash"]