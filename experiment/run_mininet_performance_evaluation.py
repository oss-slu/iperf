import csv
from subprocess import PIPE, STDOUT

from mininet.net import Mininet
from mininet.node import OVSBridge
from mininet.topo import *
from mininet.topolib import TreeTopo, TorusTopo

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

    topo_options = dict(option.split("=") for option in run["Topology options"].split(",")) if run["Topology options"] else {}
    topo_options = {key: int(value) for key, value in topo_options.items()}
    topo = topo_cls(**topo_options)

    net = Mininet(topo=topo, switch=OVSBridge, controller=None)
    net.start()

    h1 = net.hosts[0]
    server = h1.popen("../src/iperf3", "-s", stdout=PIPE, stderr=STDOUT, text=True)

    try:
        for host in net.hosts[1:]:
            out_subdir = f"{run['Topology']}_{host}_{'_'.join(f'{key}{value}' for key, value in topo_options.items())}"
            proc = host.popen(
                "python3",
                "-u",
                "run_mininet_evaluation.py",
                h1.IP(),
                out_subdir,
                stdout=PIPE,
                stderr=STDOUT,
                text=True,
            )

            for line in proc.stdout:
                print(f"[{host.name}] {line}", end="", flush=True)

            return_code = proc.wait()
            if return_code != 0:
                raise RuntimeError(f"run_mininet_evaluation.py failed on {host.name} with exit code {return_code}")
    finally:
        server.terminate()
        server.wait(timeout=5)
        net.stop()