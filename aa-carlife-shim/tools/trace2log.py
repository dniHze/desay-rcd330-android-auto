#!/usr/bin/env python3
"""Turn an `strace -tt -s 8192 -e trace=write,writev` capture into the process's own log.

The SVP services and apps run with --debug-output none but still write their logs, to stdout
(block-buffered, 4 KB per write) and stderr (line by line). Each output line is prefixed with
the time of the write() that carried it (exact for stderr, the block's flush time for stdout)
and the fd. Usage: trace2log.py <trace> [> out.log]
"""
import ast
import re
import sys

WRITE = re.compile(r'^(?:\[pid\s+\d+\] )?(\S+) write\(([12]), "(.*)", \d+\) = \d+$')


def decode(s):
    # strace prints C-style escapes; literal_eval of a bytes literal undoes them.
    try:
        return ast.literal_eval('b"' + s.replace('"', '\\"').replace('\\\\"', '\\"') + '"').decode("utf-8", "replace")
    except (SyntaxError, ValueError):
        return s.replace("\\r", "").replace("\\n", "\n")


for raw in open(sys.argv[1], errors="replace"):
    m = WRITE.match(raw.rstrip("\n"))
    if not m:
        continue
    t, fd, text = m.groups()
    for line in decode(text).replace("\r", "").split("\n"):
        if line.strip():
            print("%s %s %s" % (t, fd, re.sub(r"^\(\d+\|0x[0-9A-Fa-f]+\) ", "", line)))
