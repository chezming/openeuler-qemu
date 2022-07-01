#!/bin/bash
./checkhmcode.sh
result=$?
right=0
if [ $result != $right ];then
	exit 1
fi
./configure --target-list=sw64-softmmu  --enable-spice --enable-kvm  --extra-cflags="-fpic -fPIC"  --extra-ldflags="-fpic -fPIC" --enable-debug --enable-seccomp  --disable-werror
