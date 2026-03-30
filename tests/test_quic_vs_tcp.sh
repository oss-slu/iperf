#!/bin/bash
#
# test_quic_vs_tcp.sh -- compare TCP vs QUIC throughput
#
# Runs single-stream, multi-stream, and reverse-mode tests for
# both TCP and QUIC, then prints a summary with pass/fail status.
# Multi-stream tests also verify that every stream carries non-zero
# throughput (not just the first one).
#
# Usage:
#   ./tests/test_quic_vs_tcp.sh
#
# Run from the iperf directory (where src/iperf3 lives).

set -e

IPERF=./src/iperf3
PORT=5220
DURATION=5
PARALLEL=4
TOTAL_TESTS=0
TOTAL_PASS=0
TOTAL_FAIL=0

if [ ! -x "$IPERF" ]; then
    echo "ERROR: $IPERF not found; run make first" >&2
    exit 1
fi

MSQUIC_LIB="$(cd ../msquic/build/bin/Release 2>/dev/null && pwd)" || true
if [ -n "$MSQUIC_LIB" ]; then
    export DYLD_LIBRARY_PATH="$MSQUIC_LIB:${DYLD_LIBRARY_PATH:-}"
    export LD_LIBRARY_PATH="$MSQUIC_LIB:${LD_LIBRARY_PATH:-}"
fi

if ! $IPERF --help 2>&1 | grep -q -- '--quic'; then
    echo "ERROR: iperf3 was not built with QUIC support" >&2
    exit 1
fi

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

record() {
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if [ "$1" -eq 0 ]; then
	TOTAL_PASS=$((TOTAL_PASS + 1))
    else
	TOTAL_FAIL=$((TOTAL_FAIL + 1))
    fi
}

run_test() {
    local label="$1"
    local proto="$2"
    local streams="$3"
    local extra="$4"

    echo "------------------------------------------------------------"
    echo "  $label"
    echo "------------------------------------------------------------"

    if [ -n "$proto" ]; then
	$IPERF -s $proto -p $PORT --one-off 2>&1 &
    else
	$IPERF -s -p $PORT --one-off 2>&1 &
    fi
    SERVER_PID=$!
    sleep 2

    local outfile
    outfile=$(mktemp)
    if [ -n "$proto" ]; then
	$IPERF -c 127.0.0.1 $proto -p $PORT -P "$streams" \
	    -t $DURATION $extra 2>&1 | tee "$outfile"
    else
	$IPERF -c 127.0.0.1 -p $PORT -P "$streams" \
	    -t $DURATION $extra 2>&1 | tee "$outfile"
    fi
    local rc=$?

    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    if [ $rc -ne 0 ]; then
	echo "FAIL: client exited with code $rc"
	rm -f "$outfile"
	return 1
    fi

    # For multi-stream runs, check that every stream has non-zero bytes
    if [ "$streams" -gt 1 ]; then
	local zero_streams
	zero_streams=$(grep -c '0\.00 Bytes' "$outfile" 2>/dev/null || true)
	if [ "$zero_streams" -gt 0 ]; then
	    echo "FAIL: $zero_streams stream(s) reported 0.00 Bytes"
	    rm -f "$outfile"
	    return 1
	fi
    fi

    rm -f "$outfile"
    echo ""
    return 0
}

echo ""
echo "============================================================"
echo "  TCP vs QUIC -- single stream, ${DURATION}s"
echo "============================================================"
echo ""

run_test "TCP  (1 stream)" "" 1; record $?
sleep 1
run_test "QUIC (1 stream)" "--quic" 1; record $?

echo ""
echo "============================================================"
echo "  TCP vs QUIC -- ${PARALLEL} parallel streams, ${DURATION}s"
echo "============================================================"
echo ""

run_test "TCP  ($PARALLEL streams)" "" $PARALLEL; record $?
sleep 1
run_test "QUIC ($PARALLEL streams)" "--quic" $PARALLEL; record $?

echo ""
echo "============================================================"
echo "  QUIC reverse mode (-R)"
echo "============================================================"
echo ""

run_test "QUIC reverse (1 stream)" "--quic" 1 "-R"; record $?
sleep 1
run_test "QUIC reverse ($PARALLEL streams)" "--quic" $PARALLEL "-R"; record $?

echo ""
echo "============================================================"
echo "  QUIC -P 8 stress"
echo "============================================================"
echo ""

run_test "QUIC (8 streams)" "--quic" 8; record $?

echo ""
echo "============================================================"
echo "  Summary"
echo "============================================================"
echo ""
echo "  Passed: $TOTAL_PASS / $TOTAL_TESTS"
echo "  Failed: $TOTAL_FAIL / $TOTAL_TESTS"
echo ""
if [ $TOTAL_FAIL -gt 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
exit 0
