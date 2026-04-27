# Mininet Setup

This document describes how Mininet was set up for this experiment, so that the setup can be replicated for experiments.

## OS Setup

The experiment was run on Ubuntu 24.04.4 LTS on Windows Subsystem for Linux.

## Prerequisites and Mininet Installation

Mininet was installed by following the steps described at the Mininet setup page at https://mininet.org/download/. Option 3 on the site was followed: installing from a package. Mininet was installed with the command

```bash
sudo apt install mininet
```

The other steps on the page were followed, including installing and running Open vSwitch with

```bash
sudo apt-get install openvswitch-switch
sudo service openvswitch-switch start
```

and installing additional software with 

```
git clone https://github.com/mininet/mininet
mininet/util/install.sh -fw
```

Following this, the installed version of Mininet was `2.3.0.dev6`.

## Mininet Topology Setup

No additional setup for Mininet or any topologies was done outside of the scripts provided here.