#!/bin/bash
# HOST TEST helper: a compiler that fails for room_test.cpp and race_protocol.cpp syntax checks, else g++.
for a in "$@"; do case "$a" in *room_test.cpp|*core/race_protocol.cpp) [ "$1" = "-std=gnu++11" ] || [ "$a" = "${a%room_test.cpp}" ] || { echo "injected compiler failure" >&2; exit 1; };; esac; done
for a in "$@"; do case "$a" in *core/race_protocol.cpp) for b in "$@"; do [ "$b" = "-fsyntax-only" ] && { echo "injected syntax failure" >&2; exit 1; }; done;; esac; done
exec g++ "$@"
