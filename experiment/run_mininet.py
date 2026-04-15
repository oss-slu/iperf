from mininet.net import Mininet
from mininet.topo import *
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
tree = topo_cls(**topo_options)

net = Mininet(topo=tree, switch=OVSBridge, controller=None)
net.start()
h1, h2  = net.hosts[0], net.hosts[1]
print(h1.cmd('../src/iperf3 -s &'))
print(h2.cmd(f'python3 run_experiment.py {h1.IP()}'))
net.stop()
