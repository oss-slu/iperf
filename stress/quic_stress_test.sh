#!/usr/bin/env bash
#
# QUIC stress tests using Linux network namespaces and tc (netem + tbf).
#
# Test 1: Parallelism sweep (P = 1, 2, 4, 16, 64) at fixed delay and no loss.
# Test 2: Combined delay and loss grid (P = 4).
#
# Writes one JSON file per run under /iperf/stress_results/ (Docker) or
# ./stress_results/ when paths are set up the same way in the container.
#
set -euo pipefail

IPERF="/iperf/src/iperf3"
RESULTS="/iperf/stress_results"
DURATION="${DURATION:-5}"
BW="${BW_MBIT:-100}"

mkdir -p "$RESULTS"
rm -f "$RESULTS"/*.json "$RESULTS"/*.csv

cleanup_all() {
    for ns in $(ip netns list 2>/dev/null | awk '{print $1}'); do
        ip netns pids "$ns" 2>/dev/null | xargs -r kill -9 2>/dev/null || true
        ip netns del "$ns" 2>/dev/null || true
    done
    sleep 1
}
trap cleanup_all EXIT

setup_link() {
    local delay="$1" loss="$2" bw="$3"

    cleanup_all

    ip netns add srv
    ip netns add cli
    ip link add v-srv type veth peer name v-cli
    ip link set v-srv netns srv
    ip link set v-cli netns cli

    ip netns exec srv ip addr add 10.0.0.1/24 dev v-srv
    ip netns exec srv ip link set v-srv up
    ip netns exec srv ip link set lo up

    ip netns exec cli ip addr add 10.0.0.2/24 dev v-cli
    ip netns exec cli ip link set v-cli up
    ip netns exec cli ip link set lo up

    local has_netem=false has_tbf=false
    if [ "$delay" -gt 0 ] || [ "$loss" != "0" ]; then
        has_netem=true
    fi
    if [ "$bw" -gt 0 ] && [ "$bw" -lt 10000 ]; then
        has_tbf=true
    fi

    local burst=0
    if $has_tbf; then
        burst="$((bw * 1000 / 8))"
        [ "$burst" -lt 1600 ] && burst=1600
    fi

    for pair in "srv:v-srv" "cli:v-cli"; do
        local ns="${pair%%:*}" dev="${pair#*:}"
        if $has_tbf && $has_netem; then
            ip netns exec "$ns" tc qdisc add dev "$dev" root handle 1: tbf \
                rate "${bw}mbit" burst "$burst" latency 50ms 2>/dev/null || true
            ip netns exec "$ns" tc qdisc add dev "$dev" parent 1:1 handle 10: netem \
                delay "${delay}ms" loss "${loss}%" 2>/dev/null || true
        elif $has_netem; then
            ip netns exec "$ns" tc qdisc add dev "$dev" root netem \
                delay "${delay}ms" loss "${loss}%" 2>/dev/null || true
        elif $has_tbf; then
            ip netns exec "$ns" tc qdisc add dev "$dev" root tbf \
                rate "${bw}mbit" burst "$burst" latency 50ms 2>/dev/null || true
        fi
    done

    ip netns exec cli ping -c 5 -W 5 10.0.0.1 >/dev/null 2>&1 || {
        echo "WARNING: ping failed (expected at high loss)"
    }
}

wait_server() {
    local port="${1:-5201}"
    for _ in $(seq 1 50); do
        if ip netns exec srv ss -tln 2>/dev/null | grep -q ":${port} "; then
            return 0
        fi
        sleep 0.1
    done
    echo "WARNING: server may not be ready on port $port"
    return 1
}

kill_server() {
    ip netns exec srv pkill -9 -f iperf3 2>/dev/null || true
    sleep 1
}

run_one() {
    local par="$1" delay="$2" loss="$3" bw="$4" tag="$5"

    local logfile="${RESULTS}/${tag}_quic_p${par}_bw${bw}_d${delay}_l${loss}.json"

    echo -n "  QUIC P=$par BW=${bw}M d=${delay}ms loss=${loss}% ... "

    ip netns exec srv $IPERF -s --quic -D 2>/dev/null
    wait_server

    ip netns exec cli $IPERF -c 10.0.0.1 --quic \
        -P "$par" -t "$DURATION" -J \
        --logfile "$logfile" 2>/dev/null
    local rc=$?

    sleep 2
    kill_server

    if [ $rc -ne 0 ]; then
        echo "FAIL (exit $rc)"
        return 1
    fi
    if [ ! -s "$logfile" ]; then
        echo "FAIL (no output)"
        return 1
    fi
    if grep -q '"error"' "$logfile" 2>/dev/null; then
        echo "FAIL (iperf error)"
        return 1
    fi

    local mbps
    mbps=$(python3 -c "\
import json, sys
with open('$logfile') as f:
    d = json.load(f)
end = d.get('end', {})
sent = end.get('sum_sent', {}).get('bits_per_second', 0)
recv = end.get('sum_received', {}).get('bits_per_second', 0)
print(f'{max(sent, recv)/1e6:.1f}')
" 2>/dev/null || echo "?")

    echo "PASS (${mbps} Mbit/s)"
    return 0
}

# --- Test 1: parallelism sweep ---
run_parallelism_sweep() {
    echo "============================================================"
    echo " QUIC Parallelism Sweep (BW=${BW}M, delay=10ms, no loss)"
    echo "============================================================"

    local pass=0 fail=0

    for par in 1 2 4 16 64; do
        setup_link 10 0 "$BW"
        if run_one "$par" 10 0 "$BW" "par"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
        fi
    done

    echo ""
    echo "Parallelism sweep: $pass passed, $fail failed"
    echo ""
}

# --- Test 2: delay x loss ---
run_combined_stress() {
    echo "============================================================"
    echo " QUIC Combined Stress (delay x loss, BW=${BW}M)"
    echo "============================================================"

    local pass=0 fail=0

    for delay in 10 100; do
        echo ""
        echo "--- delay = ${delay}ms ---"
        for loss in 0 1 3 7 15; do
            setup_link "$delay" "$loss" "$BW"
            if run_one 4 "$delay" "$loss" "$BW" "stress"; then
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
            fi
        done
    done

    echo ""
    echo "Combined stress: $pass passed, $fail failed"
    echo ""
}

# --- main ---
echo ""
run_parallelism_sweep
run_combined_stress

echo "============================================================"
echo " All results in $RESULTS/"
echo "============================================================"
