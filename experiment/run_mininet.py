from mininet.net import Mininet
from mininet.topolib import TreeTopo
from mininet.node import OVSBridge

tree2 = TreeTopo(depth=2,fanout=2)
net = Mininet(topo=tree2, switch=OVSBridge, controller=None)
net.start()
h1, h2  = net.hosts[0], net.hosts[1]
print(h1.cmd('iperf -s &'))
print(h2.cmd(f'iperf -c {h1.IP()} -t 10'))
net.stop()
