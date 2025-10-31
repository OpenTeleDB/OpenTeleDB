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

if [ ! -f "$pid_file" ]; then
    echo "Warning: PID file $pid_file does not exist."
    exit 0
fi

pid=$(cat "$pid_file")

if ps -p "$pid" > /dev/null 2>&1; then
    echo "Killing xproxy process with PID $pid..."
    kill "$pid"
    sleep 2
    if ps -p "$pid" > /dev/null 2>&1; then
        echo "Process $pid is still running. Sending SIGKILL..."
        kill -9 "$pid"
    else
        echo "Process $pid has been successfully killed."
    fi
else
    echo "Process with PID $pid is not running."
fi