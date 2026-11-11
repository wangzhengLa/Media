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
	localip=`ifconfig | grep "inet addr:" | grep -v 127.0.0.1 | awk '{ print $2 }' | cut -c 6-`
	gatewayip=`route -n | grep "0.0.0.0" | awk '{ print $2 }' | grep -v "0.0.0.0"`
	dnsip=`cat /etc/resolv.conf |grep nameserver |awk '{ print $2 }'`

	echo -e "localip:$localip"
	echo -e "gatewayip:$gatewayip"
	echo -e "dnsip:$dnsip"
}

function netcardcheck() {
	myping $localip
	if [ $? -eq 1 ];
	then
		echo -e "$localip can be reached";
	else
		echo -e "$localip is not reachable";
	fi
}

function gatewaycheck() {
	myping $gatewayip
	if [ $? -eq 1 ];
	then
		echo -e "Reachable from localip:$localip to gatewayip:$gatewayip\n\n";
	else
		echo -e "localip:$localip to gatewayip:$gatewayip unreachable\n\n"
	fi
}

function internetipcheck() {
	internetip=114.114.114.114
	myping $internetip
	if [ $? -eq 1 ];
	then
		echo -e "Reachable from localip:$localip to internetip:$internetip\n\n";
	else
		echo -e "localip:$localip to internetip:$internetip unreachable\n\n"
	fi
}

function domaincheck() {
	domainname=www.baidu.com
	myping $domainname
	if [ $? -eq 1 ];
	then
		echo -e "Reachable from localip:$localip to domainname:$domainname\n\n";
	else
		echo -e "localip:$localip to domainname:$domainname unreachable\n\n"
	fi
}

function myservicecheck() {
	read -p "please enter the detected host ip:(for example:192.168.0.100)" a
	myping a
	if [$? -eq 1 ];
	then
		echo -e "Reachable from localip:$localip to internetip:$a\n\n";
	else
		echo -e "localip:$localip to internetip:$a unreachable\n\n"
	fi
}

function main() {
	echo -e "1.Basic network detection\n2.Specified service network connectivity detection\n3.exit"
	read -p "Your choice: " opt
	case $opt in
	1)
		baseinf_get
		netcardcheck
		gatewaycheck
		internetipcheck
		domaincheck
	;;
	2)
		myservicecheck
	;;
	3)
	exit
	;;
	*)
	echo "Please enter 1/2/3, others are invalid"
	;;
	esac
}

main
