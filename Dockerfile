FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    g++ \
    make \
    libncurses5-dev \
    libncursesw5-dev \
    librt-dev 2>/dev/null || true \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN make all

CMD ["./arbiter_bin"]