#!/usr/bin/env bash
# Builds and runs the upsampler check. From the work tree:
#   docker run --rm -v "$PWD":/w debian:bookworm bash /w/aa-bridge/tools/resample_test/run.sh
command -v g++ > /dev/null || { apt-get update -qq > /dev/null && apt-get install -y -qq g++ > /dev/null 2>&1; }
g++ -std=gnu++11 -O2 -I/w/aa-bridge/src -o /tmp/resample_test /w/aa-bridge/tools/resample_test/main.cpp && /tmp/resample_test
