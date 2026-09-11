# TestHub — multi-stage image
#   docker build -t testhub .
#   docker run --rm -p 8080:8080 testhub
#
# Runtime layout matches TESTHUB_HOME: /opt/testhub/{bin,runners,specs}

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt .
COPY cmake cmake
COPY src src
COPY web web
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DTESTHUB_BUILD_TESTS=OFF \
        -DTESTHUB_WARNINGS_AS_ERRORS=ON \
    && cmake --build build -j

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/testhub --shell /usr/sbin/nologin testhub \
    && mkdir -p /var/lib/testhub/results /var/lib/testhub/schedules /etc/testhub \
    && chown -R testhub:testhub /var/lib/testhub
COPY --from=build /src/build/testhub /opt/testhub/bin/testhub
COPY runners /opt/testhub/runners
COPY specs /opt/testhub/specs
COPY packaging/docker/testhub.json /etc/testhub/testhub.json
ENV TESTHUB_HOME=/opt/testhub \
    PATH="/opt/testhub/bin:${PATH}"
USER testhub
WORKDIR /var/lib/testhub
EXPOSE 8080
VOLUME ["/var/lib/testhub"]
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=4s --start-period=10s --retries=3 \
    CMD python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/api/v1/health', timeout=3)"
ENTRYPOINT ["/opt/testhub/bin/testhub"]
CMD ["--config", "/etc/testhub/testhub.json"]
