#!/bin/sh
set -eu

CFLAGS='-Wall -Wextra -Wpedantic -Werror -g -std=c11 -D_POSIX_C_SOURCE=200809L -pthread'
LIBCONFIG_FLAGS="$(pkg-config --cflags --libs libconfig)"

gcc $CFLAGS server.c unixds.c inetds2.c soapds.c proto.c runtime_state.c $LIBCONFIG_FLAGS -o server
gcc $CFLAGS admin_client.c $LIBCONFIG_FLAGS -o admin_client
gcc $CFLAGS ordinary_client.c $LIBCONFIG_FLAGS -o ordinary_client

python3 -m py_compile ordinary_client_alt.py

printf 'build finalizat\n'
