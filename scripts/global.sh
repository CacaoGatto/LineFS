#!/bin/bash
PROJ_DIR="/path/to/host/proj_root"     # Host's project root directory path.
NIC_PROJ_DIR="/path/to/nic/proj_root" # NIC's project root directory path.

SYS=$(gcc -dumpmachine)
if [ $SYS = "aarch64-linux-gnu" ]; then
	PINNING="" # There is only one socket in the SmartNIC.
else
	PINNING="numactl -N0 -m0"
fi

HOST_1="libra06"
HOST_2="libra08"
HOST_3="libra09"

NIC_1="libra06-nic-rdma"
NIC_2="libra08-nic-rdma"
NIC_3="libra09-nic-rdma"

buildAssise() {
	(
		cd "$PROJ_DIR" || exit
		make kernfs-assise && make libfs-assise || exit 1
	)
}

buildLineFS() {
	(
		cd "$PROJ_DIR" || exit
		make kernfs-linefs && make libfs-linefs || exit 1
	)
}
