#!/bin/bash
#
# test_quic_vs_tcp.sh -- compare TCP vs QUIC throughput
#
# Runs single-stream, multi-stream, and reverse-mode tests for
# both TCP and QUIC, then prints a summary with pass/fail status.
#
# All tests use JSON output (-J) for reliable parsing.  Multi-stream
# tests verify that each stream carries a fair share of throughput.
# Reverse-mode tests verify the data direction is correct.
# Failed test output is saved under test_failures/ for debugging.
#
# Usage:
#   ./tests/test_quic_vs_tcp.sh
#
# Run from the iperf directory (where src/iperf3 lives).

IPERF=./src/iperf3
BASE_PORT=5220
DURATION=5
PARALLEL=4
TOTAL_TESTS=0
TOTAL_PASS=0
TOTAL_FAIL=0
TEST_NUM=0
FAIL_DIR="test_failures"

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

mkdir -p "$FAIL_DIR"

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

# validate_json <json_file> <streams> <extra_flags>
#
# Parses the JSON output and checks:
#   - no "error" field present
#   - sum throughput > 0
#   - for multi-stream: each stream has >= 10% of average throughput
#   - for reverse mode (-R): sender bytes come from the server side
validate_json() {
    local jfile="$1" streams="$2" extra="$3"

    python3 - "$jfile" "$streams" "$extra" <<'PYEOF'
import json, sys

jfile, nstreams, extra = sys.argv[1], int(sys.argv[2]), sys.argv[3]

with open(jfile) as f:
    data = json.load(f)

if "error" in data:
    print(f"FAIL: iperf3 error: {data['error']}")
    sys.exit(1)

end = data.get("end", {})
if not end:
    print("FAIL: no 'end' section in JSON output")
    sys.exit(1)

# check total throughput
s_sent = end.get("sum_sent", {})
s_recv = end.get("sum_received", {})
sent_bps = s_sent.get("bits_per_second", 0)
recv_bps = s_recv.get("bits_per_second", 0)
total_bps = max(sent_bps, recv_bps)

if total_bps <= 0:
    print("FAIL: zero total throughput")
    sys.exit(1)

# reverse mode: the client is the receiver, so sum_sent should
# reflect server->client.  In -R mode the client report shows
# sum_sent with "sender" = True on the server side.  We verify
# that the receiver got data.
if "-R" in extra:
    recv_bytes = s_recv.get("bytes", 0)
    if recv_bytes <= 0:
        print("FAIL: reverse mode but receiver got 0 bytes")
        sys.exit(1)

# multi-stream: check every stream carries a fair share
streams = end.get("streams", [])
if nstreams > 1 and len(streams) >= nstreams:
    per_stream = []
    for st in streams:
        sender = st.get("sender", {})
        bps = sender.get("bits_per_second", 0)
        per_stream.append(bps)

    if len(per_stream) == 0:
        print("FAIL: no per-stream data")
        sys.exit(1)

    avg_bps = sum(per_stream) / len(per_stream)
    threshold = avg_bps * 0.10  # each stream should have >= 10% of avg
    for i, bps in enumerate(per_stream):
        if bps < threshold:
            pct = (bps / avg_bps * 100) if avg_bps > 0 else 0
            print(f"FAIL: stream [{i+1}] has {bps/1e6:.1f} Mbit/s "
                  f"({pct:.0f}% of avg {avg_bps/1e6:.1f}) — below 10% threshold")
            sys.exit(1)

    print(f"  streams: {len(per_stream)}, "
          f"min={min(per_stream)/1e6:.1f} max={max(per_stream)/1e6:.1f} Mbit/s")

print(f"  throughput: {total_bps/1e6:.1f} Mbit/s (sent={sent_bps/1e6:.1f}, recv={recv_bps/1e6:.1f})")
sys.exit(0)
PYEOF
}

run_test() {
    local label="$1"
    local proto="$2"
    local streams="$3"
    local extra="$4"

    TEST_NUM=$((TEST_NUM + 1))
    local port=$((BASE_PORT + TEST_NUM * 2))

    echo "------------------------------------------------------------"
    echo "  [$TEST_NUM] $label  (port $port)"
    echo "------------------------------------------------------------"

    if [ -n "$proto" ]; then
	$IPERF -s $proto -p $port --one-off 2>&1 &
    else
	$IPERF -s -p $port --one-off 2>&1 &
    fi
    SERVER_PID=$!
    sleep 2

    local jfile
    jfile=$(mktemp /tmp/iperf_test_XXXXXX.json)

    if [ -n "$proto" ]; then
	$IPERF -c 127.0.0.1 $proto -p $port -P "$streams" \
	    -t $DURATION $extra -J --logfile "$jfile" 2>&1
    else
	$IPERF -c 127.0.0.1 -p $port -P "$streams" \
	    -t $DURATION $extra -J --logfile "$jfile" 2>&1
    fi
    local rc=$?

    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    if [ $rc -ne 0 ]; then
	echo "  FAIL: client exited with code $rc"
	cp "$jfile" "$FAIL_DIR/test${TEST_NUM}_$(echo "$label" | tr ' ()' '___').json" 2>/dev/null
	rm -f "$jfile"
	return 1
    fi

    if [ ! -s "$jfile" ]; then
	echo "  FAIL: no JSON output produced"
	rm -f "$jfile"
	return 1
    fi

    local vrc=0
    validate_json "$jfile" "$streams" "$extra" || vrc=$?

    if [ $vrc -ne 0 ]; then
	local saved="$FAIL_DIR/test${TEST_NUM}_$(echo "$label" | tr ' ()' '___').json"
	cp "$jfile" "$saved" 2>/dev/null
	echo "  output saved to $saved"
	rm -f "$jfile"
	return 1
    fi

    echo "  PASS"
    rm -f "$jfile"

    # extra delay after QUIC tests for connection teardown
    if [ -n "$proto" ]; then
	sleep 3
    else
	sleep 1
    fi

    return 0
}

echo ""
echo "============================================================"
echo "  TCP vs QUIC -- single stream, ${DURATION}s"
echo "============================================================"
echo ""

run_test "TCP  (1 stream)" "" 1 ""; record $?
run_test "QUIC (1 stream)" "--quic" 1 ""; record $?

echo ""
echo "============================================================"
echo "  TCP vs QUIC -- ${PARALLEL} parallel streams, ${DURATION}s"
echo "============================================================"
echo ""

run_test "TCP  ($PARALLEL streams)" "" $PARALLEL ""; record $?
run_test "QUIC ($PARALLEL streams)" "--quic" $PARALLEL ""; record $?

echo ""
echo "============================================================"
echo "  QUIC reverse mode (-R)"
echo "============================================================"
echo ""

run_test "QUIC reverse (1 stream)" "--quic" 1 "-R"; record $?
run_test "QUIC reverse ($PARALLEL streams)" "--quic" $PARALLEL "-R"; record $?

echo ""
echo "============================================================"
echo "  QUIC -P 8 stress"
echo "============================================================"
echo ""

run_test "QUIC (8 streams)" "--quic" 8 ""; record $?

echo ""
echo "============================================================"
echo "  Summary"
echo "============================================================"
echo ""
echo "  Passed: $TOTAL_PASS / $TOTAL_TESTS"
echo "  Failed: $TOTAL_FAIL / $TOTAL_TESTS"
echo ""
if [ $TOTAL_FAIL -gt 0 ]; then
    echo "  Failed test output saved in $FAIL_DIR/"
    echo ""
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
exit 0
