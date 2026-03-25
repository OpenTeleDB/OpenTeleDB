#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: $0 <config_file_path>"
    exit 1
fi

config_file="$1"

if [ ! -f "$config_file" ]; then
    echo "Error: Config file $config_file does not exist."
    exit 1
fi

pid_file=$(grep -E '^\s*pid_file\s+' "$config_file" | sed 's/^\s*pid_file\s*"\([^"]*\)".*$/\1/')

if [ -z "$pid_file" ]; then
    echo "Error: pid_file path not found in $config_file."
    exit 1
fi

if [ -f "$pid_file" ]; then
    pid=$(cat "$pid_file")
    if ps -p "$pid" > /dev/null 2>&1; then
        echo "xproxy process with PID $pid is already running. Exiting."
        exit 0
    fi
fi

echo "Starting xproxy with config: $config_file"

script_dir=$(dirname "$(realpath "$0")")
xproxy_bin="$script_dir/xproxy"
xproxy_lib="$script_dir/../lib"
if [ -f "$xproxy_bin" ]; then
    LD_LIBRARY_PATH=$xproxy_lib $xproxy_bin "$config_file"
else
    xproxy "$config_file" 
fi