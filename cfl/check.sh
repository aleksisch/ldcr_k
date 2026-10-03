#!/bin/sh
# cfl/check.sh <build dir> <out dir> <k> <ll> [ldc flags...]: L_DCR_k by CFL-reachability
# (cfl/ldcr.py) against ldc's worklist solver, fact by fact, contexts included.
set -e
build=$1 out=$2 k=$3 ll=$4
shift 4
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
docker run --rm -u "$(id -u):$(id -g)" -v "$root":/w -w /w ldc-dev sh -c "
  $build/ldc -stat=false -ldc-k=$k -ldc-mode=ldcr $* -ldc-export=$out $ll > /dev/null &&
  $build/ldc -stat=false -ldc-k=$k -ldc-mode=ldcr $* -ldc-facts=$out/worklist.txt $ll | grep ^Solver"
docker run --rm -u "$(id -u):$(id -g)" -v "$root/third_party/CFPQ_PyAlgo":/app -v "$root":/w \
  -w /w cfpq/py_algo:latest -c "python3 cfl/ldcr.py $out -k $k --facts $out/cfl.txt"
# SVF numbers some values differently from run to run.
norm() { sed -E "s/::v[0-9]+@/::v@/g; s/::o[0-9]+:/::o:/g; s/(^| -> )o[0-9]+:/\1o:/g; s/ -> o[0-9]+(\[)/ -> o\1/" "$1" | LC_ALL=C sort; }
norm "$out/worklist.txt" > "$out/worklist.norm"
norm "$out/cfl.txt" > "$out/cfl.norm"
if cmp -s "$out/worklist.norm" "$out/cfl.norm"; then
  echo "same: $(wc -l < "$out/cfl.txt") facts"
else
  echo "DIFFERENT: $(LC_ALL=C comm -23 "$out/worklist.norm" "$out/cfl.norm" | wc -l) only worklist," \
       "$(LC_ALL=C comm -13 "$out/worklist.norm" "$out/cfl.norm" | wc -l) only CFL"
  exit 1
fi
