#!/bin/bash
set +e
cd "$1"
: > /tmp/p.log
for s in build_wabt build_kernel build_filesystem build_iso; do
    printf '=== %s ===\n' "$s" >> /tmp/p.log
    /usr/bin/time -f 'wall=%es user=%Us sys=%Ss maxrss=%MkB' \
        bash "build/$s.sh" >> /tmp/p.log 2>&1
done
echo '==== FULL LOG ===='
cat /tmp/p.log
echo
echo '==== TIMINGS ONLY ===='
grep -E '^===|wall=' /tmp/p.log
