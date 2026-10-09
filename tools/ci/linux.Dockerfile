# Build from tools/ci as the context; the source tree is mounted at run time.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential ca-certificates clang cmake curl git ninja-build \
        ncompress pkg-config python3-dev python3-venv unzip zip \
    && rm -rf /var/lib/apt/lists/*

# Keep the tool source and registry at the same revision as vcpkg.json.
RUN git init /opt/vcpkg \
    && git -C /opt/vcpkg remote add origin https://github.com/microsoft/vcpkg.git \
    && git -C /opt/vcpkg fetch --depth 1 origin 9b965a116838c6cdcd36bca60d1b81b030c8ab8d \
    && git -C /opt/vcpkg checkout --detach FETCH_HEAD \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_MAX_CONCURRENCY=4 \
    CMAKE_BUILD_PARALLEL_LEVEL=4 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /work
