#!/bin/bash
#
# test_quic_vs_tcp.sh -- compare TCP vs QUIC throughput
#
# Runs single-stream and multi-stream tests for both TCP and QUIC,
# then prints a summary with pass/fail status.
#
# Usage:
#   ./tests/test_quic_vs_tcp.sh
#
# Run from the iperf directory (where src/iperf3 lives).

set -e

IPERF=./src/iperf3
PORT=5220
DURATION=10
PARALLEL=4

if [ ! -x "$IPERF" ]; then
    echo "ERROR: $IPERF not found; run make first" >&2
    exit 1
fi

# Make sure MsQuic dylib is findable
MSQUIC_LIB="$(cd ../msquic/build/bin/Release 2>/dev/null && pwd)" || true
if [ -n "$MSQUIC_LIB" ]; then
    export DYLD_LIBRARY_PATH="$MSQUIC_LIB:${DYLD_LIBRARY_PATH:-}"
    export LD_LIBRARY_PATH="$MSQUIC_LIB:${LD_LIBRARY_PATH:-}"
fi

# Verify --quic is available
if ! $IPERF --help 2>&1 | grep -q -- '--quic'; then
    echo "ERROR: iperf3 was not built with QUIC support" >&2
    exit 1
fi

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

run_test() {
    local label="$1"
    local proto="$2"   # "" for TCP, "--quic" for QUIC
    local streams="$3" # -P value

    echo "------------------------------------------------------------"
    echo "  $label"
    echo "------------------------------------------------------------"

    # start server
    if [ -n "$proto" ]; then
	$IPERF -s $proto -p $PORT --one-off 2>&1 &
    else
	$IPERF -s -p $PORT --one-off 2>&1 &
    fi
    SERVER_PID=$!
    sleep 2

    # run client
    if [ -n "$proto" ]; then
	$IPERF -c 127.0.0.1 $proto -p $PORT -P "$streams" -t $DURATION 2>&1
    else
	$IPERF -c 127.0.0.1 -p $PORT -P "$streams" -t $DURATION 2>&1
    fi
    local rc=$?

    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    if [ $rc -ne 0 ]; then
	echo "FAIL: client exited with code $rc"
	return 1
    fi
    echo ""
    return 0
}

echo ""
echo "============================================================"
echo "  TCP vs QUIC -- single stream, ${DURATION}s"
echo "============================================================"
echo ""

run_test "TCP  (1 stream)" "" 1
TCP1=$?
sleep 1
run_test "QUIC (1 stream)" "--quic" 1
QUIC1=$?

echo ""
echo "============================================================"
echo "  TCP vs QUIC -- ${PARALLEL} parallel streams, ${DURATION}s"
echo "============================================================"
echo ""

run_test "TCP  ($PARALLEL streams)" "" $PARALLEL
TCP4=$?
sleep 1
run_test "QUIC ($PARALLEL streams)" "--quic" $PARALLEL
QUIC4=$?

echo ""
echo "============================================================"
echo "  Summary"
echo "============================================================"
echo ""

PASS=0
FAIL=0
for rc in $TCP1 $QUIC1 $TCP4 $QUIC4; do
    if [ "$rc" -eq 0 ]; then
	PASS=$((PASS + 1))
    else
	FAIL=$((FAIL + 1))
    fi
done

echo "  Passed: $PASS / 4"
echo "  Failed: $FAIL / 4"
echo ""
if [ $FAIL -gt 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
exit 0
