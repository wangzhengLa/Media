#!/bin/sh
#
# start/stop video
# version:v0.0.1 2024_08_15

load () {
	export MALLOC_ARENA_MAX=1
    video -f /etc/conf/video.json &
}

unload () {
  killall -9 video
}

case "$1" in
  start)
    load
    ;;
  stop)
    unload
    ;;
  restart)
    unload
    load
    ;;
  *)
  echo "$0 <start/stop/restart>"
  ;;
esac