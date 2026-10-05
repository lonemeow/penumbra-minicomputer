#!/bin/sh
# iss-net.sh -- run a command on a private network with a TAP device.
#
# The ISS's +eth=tap backend exchanges Ethernet frames with a Linux TAP
# device, and making one takes the right to administer the network it
# is on.  An unprivileged user has that right inside a user and network
# namespace of their own, so this script makes one, puts a TAP device
# in it that the namespace's own network stack reaches at 10.0.0.2/24,
# and runs a command there:
#
#     sw/tools/iss-net.sh make simulate SDCARD=build/copy.img RAW=1 ETH=tap
#
# The guest takes another address on that network, 10.0.0.1, say
# (`ifconfig peth0 10.0.0.1/24 up`).  While the command runs, --join
# runs another in the same namespace, from a second terminal:
#
#     sw/tools/iss-net.sh --join ping 10.0.0.1
#     sw/tools/iss-net.sh --join tcpdump -ni tap0
#
# The network has two stations, the guest and this host's stack, and no
# route beyond them.  Inside it the user appears as root; files written
# there belong to the user as usual.
#
# The guest's clock counts instructions, so its timeouts run at a
# different rate from this host's: a run here shows that packets flow,
# and says nothing about how fast.
#
# ISS_NET_TAP and ISS_NET_ADDR name the device and the host's address;
# ISS_NET_STATE is the file that records the namespace for --join.

set -eu

TAP=${ISS_NET_TAP:-tap0}
ADDR=${ISS_NET_ADDR:-10.0.0.2/24}

usage() {
	sed -n '2,/^$/s/^# \{0,1\}//p' "$0" >&2
	exit 2
}

[ $# -ge 1 ] || usage

case "$1" in
--inside)
	# Reached through unshare, below: the namespace is new and empty.
	shift
	ip link set lo up
	ip tuntap add dev "$TAP" mode tap
	ip addr add "$ADDR" dev "$TAP"
	ip link set "$TAP" up
	# exec keeps this process ID, which is what --join enters by.
	echo $$ > "$ISS_NET_STATE"
	exec "$@"
	;;
--join)
	shift
	[ $# -ge 1 ] || usage
	state=${ISS_NET_STATE:-${TMPDIR:-/tmp}/iss-net-$(id -u).pid}
	pid=$(cat "$state" 2>/dev/null) && kill -0 "$pid" 2>/dev/null || {
		echo "iss-net: no network is running" >&2
		exit 1
	}
	exec nsenter -t "$pid" -U -n --preserve-credentials "$@"
	;;
-*)
	usage
	;;
*)
	# The user's own ID names the state file, and inside the namespace
	# that ID reads as 0, so the name is settled out here.
	ISS_NET_STATE=${ISS_NET_STATE:-${TMPDIR:-/tmp}/iss-net-$(id -u).pid}
	export ISS_NET_STATE ISS_NET_TAP="$TAP" ISS_NET_ADDR="$ADDR"
	exec unshare -rn "$0" --inside "$@"
	;;
esac
