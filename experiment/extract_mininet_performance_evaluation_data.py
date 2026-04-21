import subprocess
import csv
import json

experiment_runs = []

with open("performance_evaluation_2.csv", "r", newline="") as csv_file:
    csv_reader = csv.DictReader(csv_file)
    experiment_runs = list(csv_reader)

with open("mininet_performance_evaluation_results.csv", "w", newline="") as evaluation_data_file:
    csv_writer = csv.writer(evaluation_data_file)
    csv_writer.writerow([
        "Parallelism Level",
        "Bandwidth cap (bitrate, Gbps)",
        "Protocol",
        "Packet Loss",
        "Throughput (Gbps)",
    ])

    with open("mininet_performance_evaluation.csv", "r", newline="") as mininet_csv_file:
        mininet_csv_reader = csv.DictReader(mininet_csv_file)
        mininet_experiment_runs = list(mininet_csv_reader)

    for topology_number, topology_run in enumerate(mininet_experiment_runs):
        host_count = int(topology_run["Topology options"].split(",")[0].split("=")[1]) if topology_run["Topology options"] else 0
        for host_number in range(2, host_count + 1):
            for run_number, run in enumerate(experiment_runs):
                with open(f"out_mininet_performance_eval/{topology_run['Topology']}_k{host_count}_h{host_number}/{run['Protocol']}/run{run_number}_p{run['Parallelism Level']}_b{run['Bandwidth cap (bitrate, Gbps)']}.log") as log_file:
                    json_data = json.load(log_file)
                
                # latency = json_data['end']['sum_sent'][0]['sender']['mean_rtt'] / 1000 if run['Protocol'] == 'tcp' else 0  # Convert ms to s
                loss = 0 if run['Protocol'] == 'tcp' else json_data['end']['sum']['lost_percent']
                throughput = json_data['end']['sum_sent']['bits_per_second'] / 1_000_000_000  # Convert bps to Gbps

                csv_writer.writerow([
                    run['Parallelism Level'],
                    run['Bandwidth cap (bitrate, Gbps)'],
                    run['Protocol'],
                    # latency,
                    loss,
                    throughput,
                ])