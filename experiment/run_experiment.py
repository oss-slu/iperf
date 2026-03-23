import subprocess
import csv

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
    subprocess.run(
        f"../src/iperf3 -c localhost {'' if run['Protocol'] == 'tcp' else '--' + run['Protocol']} -J -t {run['Duration (s)']} -P {run['Parallelism Level']} -b {str(int(run['Bandwidth cap (bitrate, Gbps)']) * 1_000_000_000)} --logfile out/run_{run_number}.log",
        shell=True,
        capture_output=True
    )