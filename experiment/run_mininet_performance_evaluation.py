import csv
from subprocess import PIPE, STDOUT
from time import sleep

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
    topo_tag = "_".join(f"{key}{value}" for key, value in topo_options.items())
    out_subdir_base = f"{run['Topology']}_{topo_tag}" if topo_tag else run["Topology"]

    net = Mininet(topo=topo, switch=OVSBridge, controller=None)
    net.start()

    h1 = net.hosts[0]
    server_log = f"out_mininet_performance_eval/{out_subdir_base}/server.log"
    h1.cmd(f"mkdir -p out_mininet_performance_eval/{out_subdir_base}")
    server = h1.popen("sh", "-c", f"../src/iperf3 -s > {server_log} 2>&1")
    sleep(0.2)
    if server.poll() is not None:
        raise RuntimeError(
            "iperf3 server failed to start on h1. "
            f"Server log ({server_log}):\n{h1.cmd(f'tail -n 60 {server_log}') }"
        )

    try:
        for host in net.hosts[1:]:
            sleep(0.1)
            if server.poll() is not None:
                raise RuntimeError(
                    "iperf3 server terminated before client run. "
                    f"Server log ({server_log}):\n{h1.cmd(f'tail -n 80 {server_log}') }"
                )

            out_subdir = f"{run['Topology']}_{host.name}_{topo_tag}" if topo_tag else f"{run['Topology']}_{host.name}"
            proc = host.popen(
                "python3",
                "-u",
                "run_mininet_evaluation.py",
                h1.IP(),
                out_subdir,
                "--iperf-timeout",
                "300",
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
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=5)
        net.stop()