#!/bin/sh
# lint.sh - static checks that need no toolchain and no guest.
#   sh scripts/lint.sh
set -eu
cd "$(dirname "$0")/.."

fail=0

echo "== python sources parse =="
python3 - <<'PY'
import pathlib, sys
bad = 0
for f in ["qandroid", "tools/apk.py", "tools/lpunpack.py", "frontend/viewer.py",
          "patches/apply.sh", "tests/test_tools.py", "tests/test_cli.py",
          "tests/test_hwc_proto.py", "tests/test_rootfs_build.py", "tests/test_packaging.py"]:
    try:
        compile(pathlib.Path(f).read_text(), f, "exec")
    except SyntaxError as e:
        print("%s: %s" % (f, e))
        bad = 1
sys.exit(bad)
PY

echo "== shell syntax =="
for f in scripts/*.sh packaging/*.sh; do
    sh -n "$f" || { echo "  $f: syntax error"; fail=1; }
    echo "  ok $f"
done

echo "== composer module syntax (shim headers) =="
make -s -C hwc check

echo "== yaml sanity for workflows =="
python3 - <<'PY'
import pathlib, sys
ok = True
for f in sorted(pathlib.Path(".github/workflows").glob("*.yml")):
    text = f.read_text()
    # enough structure to catch the mistakes that actually happen: tabs,
    # unindented block-scalar bodies, missing 'jobs:'
    if "\t" in text:
        print("%s: contains a tab character" % f); ok = False
    if "\njobs:" not in text and not text.startswith("jobs:"):
        print("%s: no jobs: key" % f); ok = False
    for i, line in enumerate(text.splitlines(), 1):
        if line.startswith(" ") is False and line and not line[0] in "-#" and ":" not in line:
            print("%s:%d: unindented line that is not a mapping key" % (f, i)); ok = False
print("workflows ok" if ok else "workflows FAILED")
sys.exit(0 if ok else 1)
PY

exit $fail
