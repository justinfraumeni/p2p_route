# P2P Route

OONF OLSRD2 plugin to enable multigroup routing between WiFi-P2P groups. When provided with a control interface name, p2p_route dynamically configures virtual group interfaces as they are created during a olsrd2 session.


## Setup

Check out a copy of the OONF framework:


```sh
git clone git@github.com:OLSR/OONF.git

```

Compile OONF with the p2p_route plugin in the src-plugins/generic subdirectory.


```sh
mv p2p_route OONF/src-plugins/generic
cd OONF/build
sudo make install

```

## Running olsrd2 with P2P Route


P2P Route provides a single configuration option, 'control_interface', which should be set to the name of the radio interface managing WiFi-P2P groups.

In order to enable automatic Ipv4 address generation, olsrd must be provided with local loopback as an interface parameter:


```sh
olsrd2_static global.plugin=p2p_route p2p_route.control_interface=wlan0 lo

```
