from mininet.net import Mininet
from mininet.topo import *
from mininet.topolib import TreeTopo, TorusTopo
from mininet.node import OVSBridge
import argparse

parser = argparse.ArgumentParser(description="Run iperf3 experiments in Mininet.")
parser.add_argument("--topo", help="""
                        linear|minimal|reversed|single|torus|tree[,param=value
                        ...] minimal=MinimalTopo linear=LinearTopo
                        reversed=SingleSwitchReversedTopo
                        single=SingleSwitchTopo tree=TreeTopo torus=TorusTopo""", default="minimal")
parser.add_argument("--topo-options", help="Comma-separated list of key=value pairs to pass as options to the topology class. For example: --topo-options depth=2,fanout=2")
args = parser.parse_args()

match args.topo:
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
        raise ValueError(f"Unknown topology: {args.topo}")

topo_options = dict(option.split('=') for option in args.topo_options.split(',')) if args.topo_options else {}
topo_options = {key: int(value) for key, value in topo_options.items()}  # Convert values to integers
topo = topo_cls(**topo_options)

out_subdir = f"{args.topo}_{'_'.join(f'{key}{value}' for key, value in topo_options.items())}"

net = Mininet(topo=topo, switch=OVSBridge, controller=None)
net.start()
h1 = net.hosts[0]
print(h1.cmd('../src/iperf3 -s &'))
for host in net.hosts[1:]:
    print(host.cmd(f'python3 run_experiment.py {h1.IP()} {out_subdir} &'))
net.stop()
