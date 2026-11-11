#!/bin/sh

function myping() {
	ping $1 -c 3
	if [ $? -eq 0 ];
	then
		return 1;
	else
		return 0
	fi
}

function baseinf_get() {
	gatewayip=`route -n | grep "0.0.0.0" | awk '{ print $2 }' | grep -v "0.0.0.0"`
	echo -e "gatewayip:$gatewayip"
}

function gatewaycheck() {
	myping $gatewayip
	if [ $? -eq 1 ];
	then
		echo -e "Reachable gatewayip:$gatewayip";
	else
		echo -e "Unreachable gatewayip:$gatewayip";
	fi
}

baseinf_get
gatewaycheck
