#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: $0 <config_file_path>"
    exit 1
fi

config_file="$1"

# 获取当前脚本所在的目录
script_dir=$(dirname "$(realpath "$0")")

# 停止 xproxy 进程
stop_script="$script_dir/xproxy-stop.sh"
if [ -x "$stop_script" ]; then
    echo "Stopping xproxy process..."
    "$stop_script" "$config_file"
    stop_status=$?
    if [ $stop_status -ne 0 ]; then
        echo "Error: Failed to stop xproxy process. Exit code: $stop_status"
        exit $stop_status
    fi
else
    echo "Error: $stop_script is not executable or does not exist."
    exit 1
fi

# 启动 xproxy 进程
start_script="$script_dir/xproxy-start.sh"
if [ -x "$start_script" ]; then
    echo "Starting xproxy process..."
    "$start_script" "$config_file"
    start_status=$?
    if [ $start_status -ne 0 ]; then
        echo "Error: Failed to start xproxy process. Exit code: $start_status"
        exit $start_status
    fi
else
    echo "Error: $start_script is not executable or does not exist."
    exit 1
fi

echo "xproxy process has been successfully restarted."