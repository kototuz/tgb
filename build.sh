#!/bin/sh

CC="gcc"
CFLAGS="-Wextra -Wall -Wpedantic"

mkdir -p bin

if [ "$1" = "" ]; then
    echo "You need to provide program name"
    exit 1
fi

if [ "$2" = "" ]; then
    echo "You need to provide 'BOT_TOKEN' as an argument"
    exit 1
fi

case "$1" in
    "tpilot")
        $CC $CFLAGS -I./external/ tpilot/main.c -o bin/tpilot.exe -L./external/raylib-5.5/src/ -lraylib -lcurl -lm -DBOT_TOKEN="\"$2\""
        ;;

    "troll")
        $CC $CFLAGS -I./external/ troll/main.c -o bin/troll.exe -lcurl -DBOT_TOKEN="\"$2\""
        ;;

    *)
        echo "Program '$1' not found"
        ;;
esac



