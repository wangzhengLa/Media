#!/bin/sh
# btmanager init script for AIC8800 BLE
# Usage: bt_init.sh {start|ble_start|stop|hci_attach}
#
# Hardware assumptions (verify on board):
#   - UART port:  ttyS1 (override via BTCON_SERIAL below)
#   - BT chip:    AIC8800 (H4 protocol, hciattach type=any)
#
# If AIC8800 is already driven by a kernel module (e.g. btusb or aic8800-bt),
# hci0 may already exist under /sys/class/bluetooth/. In that case this script
# skips hciattach entirely (see ble_start/start).

bt_hciattach="hciattach"
BTCON_SERIAL="${BTCON_SERIAL:-ttyAS1}"
BTCON_BAUD="${BTCON_BAUD:-1500000}"

reset_bluetooth_power() {
	# rfkill path varies by board — only write if exists
	for rf in /sys/class/rfkill/rfkill0/state /sys/class/rfkill/rfkill/state; do
		[ -e "$rf" ] && {
			echo 0 > "$rf" 2>/dev/null
			sleep 1
			echo 1 > "$rf" 2>/dev/null
			sleep 1
			break
		}
	done
}

start_hci_attach() {
	h=`ps | grep "$bt_hciattach" | grep -v grep`
	[ -n "$h" ] && killall "$bt_hciattach"

	# AIC8800 uses H4 protocol (type "any"). Aligned with S30wifi.sh:
	#   hciattach -s 1500000 /dev/ttyAS1 any 1500000 flow nosleep
	echo "trying hciattach -s $BTCON_BAUD $BTCON_SERIAL any $BTCON_BAUD flow nosleep ..."
	"$bt_hciattach" -s "$BTCON_BAUD" "$BTCON_SERIAL" any "$BTCON_BAUD" flow nosleep >/dev/null 2>&1 &

	wait_hci0_count=0
	while true
	do
		[ -d /sys/class/bluetooth/hci0 ] && break
		usleep 100000
		let wait_hci0_count++
		[ $wait_hci0_count -eq 70 ] && {
			echo "bring up hci0 failed — try different tty/baud (ls /dev/ttyS* /dev/ttyHS*)"
			exit 1
		}
	done
	echo "hci0 ready"
}

start() {
	if [ -d "/sys/class/bluetooth/hci0" ]; then
		echo "Bluetooth init has been completed!!"
	else
		start_hci_attach
	fi

	d=`ps | grep bluetoothd | grep -v grep`
	[ -z "$d" ] && {
		# bluetoothd -n &
		/etc/bluetooth/bluetoothd start
		sleep 1
	}
}

ble_start() {
	if [ -d "/sys/class/bluetooth/hci0" ]; then
		echo "Bluetooth init has been completed!!"
	else
		start_hci_attach
	fi

	hci_is_up=`hciconfig hci0 2>/dev/null | grep UP`
	[ -z "$hci_is_up" ] && {
		hciconfig hci0 up
	}
}

stop() {
	d=`ps | grep bluetoothd | grep -v grep`
	[ -n "$d" ] && {
		killall bluetoothd
		sleep 1
	}

	t=`ps | grep hcidump | grep -v grep`
	[ -n "$t" ] && killall hcidump

	hciconfig hci0 down 2>/dev/null

	h=`ps | grep "$bt_hciattach" | grep -v grep`
	[ -n "$h" ] && {
		killall "$bt_hciattach"
		usleep 500000
	}

	# XRadio-specific path — skip on AIC8800
	[ -e /proc/bluetooth/sleep/btwrite ] && echo 0 > /proc/bluetooth/sleep/btwrite

	for rf in /sys/class/rfkill/rfkill0/state /sys/class/rfkill/rfkill/state; do
		[ -e "$rf" ] && { echo 0 > "$rf" 2>/dev/null; break; }
	done

	echo "stop bluetoothd and hciattach"
}

case "$1" in
	start|"")
		start
		;;
	stop)
		stop
		;;
	ble_start)
		ble_start
		;;
	hci_attach)
		start_hci_attach
		;;
	*)
		echo "Usage: $0 {start|stop|ble_start|hci_attach}"
		exit 1
esac
