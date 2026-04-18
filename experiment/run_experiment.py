import subprocess
import csv
import argparse
from os import path, makedirs

parser = argparse.ArgumentParser(description="Run iperf3 experiments based on a CSV configuration file.")
parser.add_argument("ip", help="The IP address of the iperf3 server to connect to.")
parser.add_argument("out_subdir", help="The subdirectory within the 'out' directory where log files will be saved.")
args = parser.parse_args()

experiment_runs = []

with open("experiment_framework.csv", "r", newline="") as csv_file:
    csv_reader = csv.DictReader(csv_file)
    for row in csv_reader:
        experiment_runs.append(row)

for run_number, run in enumerate(experiment_runs):
    subprocess.run(
        f"tc qdisc add dev lo root netem delay {run['Delay (ms)']}ms loss {run['Loss (%)']}%",
        shell=True,
        capture_output=True
    )

    output_dir = f"out/{args.out_subdir}/{run['Protocol']}"
    if not path.exists(output_dir):
        makedirs(output_dir)

    subprocess.run(
        f"../src/iperf3 -c {args.ip} {'' if run['Protocol'] == 'tcp' else '--' + run['Protocol']} -J -t {run['Duration (s)']} -P {run['Parallelism Level']} -b {str(int(run['Bandwidth cap (bitrate, Gbps)']) * 1_000_000_000)} --logfile {output_dir}/run{run_number}_dur{run['Duration (s)']}_p{run['Parallelism Level']}_b{run['Bandwidth cap (bitrate, Gbps)']}_delay{run['Delay (ms)']}_l{run['Loss (%)']}.log",
        shell=True,
        capture_output=True
    )