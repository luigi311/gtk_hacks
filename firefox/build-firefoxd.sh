#!/bin/bash

gcc $( pkg-config --cflags gtk+-3.0 ) -shared -fPIC -o libfiregote.so firefox-daemon.c -ldl
