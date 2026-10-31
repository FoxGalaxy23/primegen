#!/bin/sh
set -eu
cd "$(dirname "$0")"
if [ ! -x ./my_browser ]; then
    echo "my_browser is missing; build first with: make -j2" >&2
    exit 1
fi
exec ./my_browser "$@"
