from mininet.net import Mininet
from mininet.topo import *
from mininet.topolib import TreeTopo, TorusTopo
from mininet.node import OVSBridge
import argparse
import csv
from subprocess import PIPE, STDOUT

experiment_runs = []

with open("mininet_performance_evaluation.csv", "r", newline="") as csv_file:
    csv_reader = csv.DictReader(csv_file)
    for row in csv_reader:
        experiment_runs.append(row)

for run_number, run in enumerate(experiment_runs):
    match run["Topology"]:
        case "linear":
            topo_cls = LinearTopo
        case "minimal":
            topo_cls = MinimalTopo
        case "reversed":
            topo_cls = SingleSwitchReversedTopo
        case "single":
            topo_cls = SingleSwitchTopo
        case "tree":
            topo_cls = TreeTopo
        case "torus":
            topo_cls = TorusTopo
        case _:
            raise ValueError(f"Unknown topology: {run['Topology']}")

    topo_options = dict(option.split('=') for option in run['Topology options'].split(',')) if run['Topology options'] else {}
    topo_options = {key: int(value) for key, value in topo_options.items()}  # Convert values to integers
    topo = topo_cls(**topo_options)

    out_subdir = f"{run['Topology']}_{'_'.join(f'{key}{value}' for key, value in topo_options.items())}"

    net = Mininet(topo=topo, switch=OVSBridge, controller=None)
    net.start()
    h1 = net.hosts[0]
    print(h1.cmd('../src/iperf3 -s &'))
    for host in net.hosts[1:]:
        proc = host.popen(
            "python3", "-u", "run_mininet_evaluation.py", h1.IP(), out_subdir,
            stdout=PIPE,
            stderr=STDOUT,
            text=True,
        )

        for line in proc.stdout:
            print(line, end="")

        return_code = proc.wait()
        if return_code != 0:
            print(f"run_mininet_evaluation.py failed with exit code {return_code}")
    net.stop()
