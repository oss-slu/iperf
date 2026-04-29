#!/usr/bin/env bash
#
# Build the Docker image for QUIC stress testing, run tests, and plot results.
# Requires Docker.  Uses a privileged container for ip netns and tc.
#
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE="iperf3-stress-quic"
RESULTS="$(pwd)/stress_results"

build() {
    echo "==> Building $IMAGE ..."
    docker build -f Dockerfile.stress -t "$IMAGE" .
}

run() {
    build
    mkdir -p "$RESULTS/graphs"

    echo "==> Running QUIC stress tests ..."
    docker run --rm --privileged \
        -v "$RESULTS:/iperf/stress_results" \
        "$IMAGE" \
        bash /iperf/stress/quic_stress_test.sh

    echo "==> Generating graphs ..."
    docker run --rm \
        -v "$RESULTS:/iperf/stress_results" \
        "$IMAGE" \
        python3 /iperf/stress/plot_stress.py
}

graphs() {
    build
    echo "==> Regenerating graphs from existing results ..."
    docker run --rm \
        -v "$RESULTS:/iperf/stress_results" \
        "$IMAGE" \
        python3 /iperf/stress/plot_stress.py
}

shell() {
    build
    docker run --rm -it --privileged \
        -v "$RESULTS:/iperf/stress_results" \
        "$IMAGE" \
        bash
}

case "${1:-run}" in
    build)  build ;;
    run)    run ;;
    graphs) graphs ;;
    shell)  shell ;;
    *)      echo "Usage: $0 {build|run|graphs|shell}"; exit 1 ;;
esac
