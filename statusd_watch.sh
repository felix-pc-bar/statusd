#!/bin/bash

FILE="/run/statusd/statusd.txt"

last=""

while true; do
	if [[ -f "$FILE" ]]; then
		val=$(cat "$FILE")
		if [[ "$val" != "$last" ]]; then
			echo "$val"
			last="$val"
		fi
	fi
	sleep 0.1
done

