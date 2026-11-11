#!/bin/sh

conf_item=$1
conf_value=$2
conf_file=$3

echo ------conf_item${conf_item}
echo ------conf_value${conf_value}
echo ------conf_file${conf_file}

sed -i "s/^${conf_item}=.*/${conf_item}=${conf_value}/" ${conf_file}
