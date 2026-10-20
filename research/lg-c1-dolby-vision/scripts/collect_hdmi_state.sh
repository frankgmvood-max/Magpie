#!/bin/sh
# Read-only diagnostic collection, executed locally on the user's TV.
# Requires an existing authorized shell. Does not enable root or change settings.
set -u

if command -v luna-send >/dev/null 2>&1; then
    printf '\nVideo-output service status:\n'
    luna-send -n 1 -f luna://com.webos.service.videooutput/getStatus '{}' || true
fi

for port in 0 1 2 3; do
    status="/proc/lg/hdmi20/port${port}/status"
    if [ -r "$status" ]; then
        printf '\nHDMI receiver port %s:\n' "$port"
        cat "$status"
    fi
done
