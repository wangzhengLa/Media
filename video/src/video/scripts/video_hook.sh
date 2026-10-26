#!/bin/sh
set -x

# 自动查找 ubus 路径
find_ubus() {
    # 优先使用系统路径
    if command -v ubus >/dev/null 2>&1; then
        echo "ubus"
    elif [ -f "/userdata/video_debug_rv1106/install/root/usr/bin/ubus" ]; then
        echo "/userdata/video_debug_rv1106/install/root/usr/bin/ubus"
    else
        echo ""
    fi
}

UBUS_BIN=$(find_ubus)
if [ -z "${UBUS_BIN}" ]; then
    echo "Error: ubus not found"
    exit 1
fi

UBUS_CMD="${UBUS_BIN} -s /tmp/ubus.sock call video.control"

grap_pic() {
	${UBUS_CMD} grap_pic "{\"file_path\":\"$1\"}" 
}

save_raw_video() {
	${UBUS_CMD} save_raw_video
}

request_idr() {
	${UBUS_CMD} request_idr
}

set_bitrate() {
	${UBUS_CMD} set_bitrate "{\"bit_rate\":$1}" 
}

start_ai() {
	${UBUS_CMD} start_ai
}

stop_ai() {
	${UBUS_CMD} stop_ai
}

start_push() {
	${UBUS_CMD} start_push
}

stop_push() {
	${UBUS_CMD} stop_push
}

start_rtsp_push() {
	${UBUS_CMD} start_rtsp_push
}

stop_rtsp_push() {
	${UBUS_CMD} stop_rtsp_push
}

start_libpeer_push() {
	${UBUS_CMD} start_libpeer_push
}

stop_libpeer_push() {
	${UBUS_CMD} stop_libpeer_push
}

start_webrtc_push() {
	${UBUS_CMD} start_webrtc_push
}

stop_webrtc_push() {
	${UBUS_CMD} stop_webrtc_push
}

start_jpeg_pub() {
	${UBUS_CMD} start_jpeg_pub
}

stop_jpeg_pub() {
	${UBUS_CMD} stop_jpeg_pub
}

start_qr_identify() {
	${UBUS_CMD} start_qr_identify
}

stop_qr_identify() {
	${UBUS_CMD} stop_qr_identify
}

show_usage() {
	echo "Usage: $0 <command> [arguments]"
	echo ""
	echo "Commands:"
	echo "  grap_pic <file_path>          - Capture picture to specified path"
	echo "  save_raw_video                - Save raw video"
	echo "  request_idr                   - Request IDR frame"
	echo "  set_bitrate <bit_rate>        - Set video bitrate"
	echo "  start_ai                      - Start AI processing"
	echo "  stop_ai                       - Stop AI processing"
	echo "  start_push                    - Start video push"
	echo "  stop_push                     - Stop video push"
	echo "  start_rtsp_push          - Start video rtsp push"
	echo "  stop_rtsp_push           - Stop video rtsp push"
	echo "  start_libpeer_push       - Start video libpeer(WebRTC) push"
	echo "  stop_libpeer_push        - Stop video libpeer(WebRTC) push"
	echo "  start_webrtc_push       - Start video official WebRTC push"
	echo "  stop_webrtc_push        - Stop video official WebRTC push"
	echo "  start_jpeg_pub                - Start JPEG publishing"
	echo "  stop_jpeg_pub                 - Stop JPEG publishing"
	echo "  start_qr_identify             - Start QR code identification"
	echo "  stop_qr_identify              - Stop QR code identification"
	echo ""
	echo "Examples:"
	echo "  $0 grap_pic /tmp/test.jpg"
	echo "  $0 set_bitrate 2000"
	echo "  $0 start_ai"
	echo ""
	echo "ubus path: ${UBUS_BIN}"
}

case "$1" in
	grap_pic)
		grap_pic "$2"
		;;
	save_raw_video)
		save_raw_video
		;;
	request_idr)
		request_idr
		;;
	set_bitrate)
		set_bitrate "$2"
		;;
	start_ai)
		start_ai
		;;
	stop_ai)
		stop_ai
		;;
	start_push)
		start_push
		;;
	stop_push)
		stop_push
		;;
	start_rtsp_push)
		start_rtsp_push
		;;
	stop_rtsp_push)
		stop_rtsp_push
		;;
	start_libpeer_push)
		start_libpeer_push
		;;
	stop_libpeer_push)
		stop_libpeer_push
		;;
	start_webrtc_push)
		start_webrtc_push
		;;
	stop_webrtc_push)
		stop_webrtc_push
		;;
	start_jpeg_pub)
		start_jpeg_pub
		;;
	stop_jpeg_pub)
		stop_jpeg_pub
		;;
	start_qr_identify)
		start_qr_identify
		;;
	stop_qr_identify)
		stop_qr_identify
		;;
	help|-h|--help)
		show_usage
		;;
	*)
		show_usage
		exit 1
		;;
esac