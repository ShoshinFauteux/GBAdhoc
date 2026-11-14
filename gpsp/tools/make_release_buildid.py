#!/usr/bin/env python3
# make_release_buildid.py MANIFEST -> 'commit|clean' or 'commit|dirty'
# Called by make_release.sh.
import json, pathlib, sys
m = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
commit = m.get("sourceCommit", "unknown")
clean = bool(m.get("workingTreeClean", False))
print("%s|%s" % (commit, "clean" if clean else "dirty"))
