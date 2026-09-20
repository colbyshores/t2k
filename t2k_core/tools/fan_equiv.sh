#!/usr/bin/env bash
# Prove text_c3d's indexed 6-vertex fans replay the EXACT triangles the old
# unindexed expansion wrote. Host build; no devkitARM needed.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -o /tmp/fan_equiv fan_equiv.cpp
/tmp/fan_equiv
