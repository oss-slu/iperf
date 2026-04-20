import argparse
import csv
import subprocess
import sys
from os import makedirs

parser = argparse.ArgumentParser(description="Run iperf3 experiments based on a CSV configuration file.")
parser.add_argument("ip", help="The IP address of the iperf3 server to connect to.")
parser.add_argument("out_subdir", help="The subdirectory within the 'out' directory where log files will be saved.")
parser.add_argument(
    "--iperf-timeout",
    type=int,
    default=180,
    help="Timeout in seconds for each iperf3 client run (default: 180).",
)
args = parser.parse_args()

experiment_runs = []
with open("performance_evaluation_2.csv", "r", newline="") as csv_file:
    csv_reader = csv.DictReader(csv_file)
    for row in csv_reader:
        experiment_runs.append(row)


def run_and_stream(cmd: str, *, timeout: int | None = None) -> subprocess.CompletedProcess:
    try:
        result = subprocess.run(
            cmd,
            shell=True,
            text=True,
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        print(f"TIMEOUT running: {cmd}", file=sys.stderr, flush=True)
        raise

    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr, flush=True)

    if result.returncode != 0:
        raise RuntimeError(f"Command failed ({result.returncode}): {cmd}")

    return result


try:
    for run_number, run in enumerate(experiment_runs):
        print(
            f"Running experiment {run_number + 1}/{len(experiment_runs)}: "
            f"Protocol={run['Protocol']}, "
            f"Parallelism={run['Parallelism Level']}, "
            f"Bandwidth Cap={run['Bandwidth cap (bitrate, Gbps)']} Gbps, ",
            flush=True,
        )

        # Idempotent: updates existing root qdisc instead of failing with "Exclusivity flag on"
        run_and_stream(
            f"tc qdisc replace dev lo root netem",
            timeout=5,
        )

        output_dir = f"out_mininet_performance_eval/{args.out_subdir}/{run['Protocol']}"
        makedirs(output_dir, exist_ok=True)

        iperf_cmd = (
            f"../src/iperf3 -c {args.ip} "
            f"{'' if run['Protocol'] == 'tcp' else '--' + run['Protocol']} "
            f"-J -P {run['Parallelism Level']} "
            f"-b {int(run['Bandwidth cap (bitrate, Gbps)']) * 1_000_000_000} "
            f"--logfile {output_dir}/run{run_number}_p{run['Parallelism Level']}"
            f"_b{run['Bandwidth cap (bitrate, Gbps)']}.log"
        )
        run_and_stream(iperf_cmd, timeout=args.iperf_timeout)

except Exception as exc:
    print(f"Experiment runner failed: {exc}", file=sys.stderr, flush=True)
    raise
finally:
    # Best-effort cleanup
    subprocess.run(
        "tc qdisc del dev lo root",
        shell=True,
        text=True,
        capture_output=True,
    )