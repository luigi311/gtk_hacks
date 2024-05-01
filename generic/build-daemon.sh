#!/bin/bash

gcc $( pkg-config --cflags gtk4 libelf gtksourceview-5 ) -D_GTK4=1 -o daemon daemon-v2.c -lwayland-client -ldl -lfontconfig `pkg-config --libs gtk4 libelf gtksourceview-5`
