import subprocess
import csv
import json

experiment_runs = []

with open("performance_evaluation.csv", "r", newline="") as csv_file:
    csv_reader = csv.DictReader(csv_file)
    for row in csv_reader:
        experiment_runs.append(row)

with open("performance_evaluation_results.csv", "w", newline="") as csv_file:
    csv_writer = csv.writer(csv_file)
    csv_writer.writerow([
        "Parallelism Level",
        "Bandwidth cap (bitrate, Gbps)",
        "Delay (ms)",
        "Protocol",
        "Packet Loss",
        "Throughput (Gbps)",
    ])

    for run_number, run in enumerate(experiment_runs):
        with open(f"out_performance_eval/{run['Protocol']}/run{run_number}_p{run['Parallelism Level']}_b{run['Bandwidth cap (bitrate, Gbps)']}_delay{run['Delay (ms)']}.log") as log_file:
            json_data = json.load(log_file)
        
        # latency = json_data['end']['sum_sent'][0]['sender']['mean_rtt'] / 1000 if run['Protocol'] == 'tcp' else 0  # Convert ms to s
        loss = 0 if run['Protocol'] == 'tcp' else json_data['end']['sum']['lost_percent']
        throughput = json_data['end']['sum_sent']['bits_per_second'] / 1_000_000_000  # Convert bps to Gbps

        csv_writer.writerow([
            run['Parallelism Level'],
            run['Bandwidth cap (bitrate, Gbps)'],
            run['Delay (ms)'],
            run['Protocol'],
            # latency,
            loss,
            throughput,
        ])