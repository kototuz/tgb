#!/bin/sh

CC="gcc"
CFLAGS="-Wextra -Wall -Wpedantic"

if [ "$1" = "" ]; then
    echo "You need to provide 'BOT_TOKEN' as an argument"
    exit 1
fi

$CC $CFLAGS main.c -o tgb -lcurl -DBOT_TOKEN="\"$1\""
