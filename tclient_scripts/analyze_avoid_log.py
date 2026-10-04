#!/usr/bin/env python3
"""Summarize avoid decisions, real outcomes and slow evaluations."""
from collections import Counter
import gzip
from pathlib import Path
import re
import sys

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / ".local/share/tclient-plus-kog/avoid-debug.log"
open_log = gzip.open if path.suffix == ".gz" else open
modes = Counter()
outcomes = []
slow = []
with open_log(path, "rt", errors="replace") as log:
    for line in log:
        if "[DECISION]" in line:
            match = re.search(r"mode=([\w+-]+)", line)
            if match:
                modes[match[1]] += 1
        if "[OUTCOME]" in line or "[SERVER-OUTCOME]" in line or "[SHOT-MISSED]" in line:
            outcomes.append(line.strip())
        if "[PERF]" in line:
            slow.append(line.strip())
print("Log:", path)
print("Decisions:", dict(modes))
print("Outcomes:", len(outcomes), "/ slow evaluations:", len(slow))
for line in outcomes[-15:] + slow[-5:]:
    print(line)
