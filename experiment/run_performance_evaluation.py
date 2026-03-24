import subprocess
import csv

experiment_runs = []

with open("performance_evaluation.csv", "r", newline="") as csv_file:
    csv_reader = csv.DictReader(csv_file)
    for row in csv_reader:
        experiment_runs.append(row)

for run_number, run in enumerate(experiment_runs):
    subprocess.run(
        f"tc qdisc add dev lo root netem delay {run['Delay (ms)']}ms",
        shell=True,
        capture_output=True
    )
    subprocess.run(
        f"../src/iperf3 -c localhost {'' if run['Protocol'] == 'tcp' else '--' + run['Protocol']} -J -P {run['Parallelism Level']} -b {str(int(run['Bandwidth cap (bitrate, Gbps)']) * 1_000_000_000)} --logfile out_performance_eval/{run['Protocol']}/run{run_number}_p{run['Parallelism Level']}_b{run['Bandwidth cap (bitrate, Gbps)']}_delay{run['Delay (ms)']}.log",
        shell=True,
        capture_output=True
    )