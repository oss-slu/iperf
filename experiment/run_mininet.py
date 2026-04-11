from mininet.net import Mininet
from mininet.topolib import TreeTopo
from mininet.node import OVSBridge

tree2 = TreeTopo(depth=2,fanout=2)
net = Mininet(topo=tree2, switch=OVSBridge, controller=None)
net.start()
h1, h2  = net.hosts[0], net.hosts[1]
print(h1.cmd('../src/iperf3 -s &'))
print(h2.cmd(f'python3 run_experiment.py {h1.IP()}'))
net.stop()
