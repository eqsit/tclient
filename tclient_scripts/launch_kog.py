#!/usr/bin/env python3
"""Launch the isolated tclient+ build, keeping the last five compressed logs."""
import gzip
import os
from pathlib import Path
import shutil
import sys
from datetime import datetime

repo = Path(__file__).resolve().parents[1]
profile = Path.home() / ".local/share/tclient-plus-kog"
profile.mkdir(parents=True, exist_ok=True)
build = repo / "build"
# Persisted profiles are separate; shared DDNet content is a read fallback.
storage = "\n".join([
    "add_path " + str(profile),
    "add_path " + str(build / "data"),
    "add_path " + str(Path.home() / ".local/share/ddnet"),
    "add_path $CURRENTDIR", "",
])
(build / "storage.cfg").write_text(storage)
logs = profile / "logs"
logs.mkdir(exist_ok=True)
current = profile / "avoid-debug.log"
if current.exists() and current.stat().st_size:
    archive = logs / (datetime.now().strftime("avoid-%Y%m%d-%H%M%S-%f.log.gz"))
    with current.open("rb") as source, gzip.open(archive, "wb") as output:
        shutil.copyfileobj(source, output)
    current.unlink()
    for old in sorted(logs.glob("avoid-*.log.gz"), reverse=True)[5:]:
        old.unlink()
os.chdir(build)
binary = build / "DDNet"
os.execv(binary, [str(binary), "logfile " + str(current), *sys.argv[1:]])
