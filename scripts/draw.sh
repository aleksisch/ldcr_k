#!/usr/bin/env bash
# Draw the PAG of a C++ file: compile it to IR as the tests do, run ldc --pag-dot with the source
# shown, demangle and render.
#   scripts/draw.sh file.cpp [out.png] [from:to...] [ldc options...]
#   scripts/draw.sh tests/cpp/fig8.cpp fig8.png i1:H::m::p main::h:H::n::this --lang DCR --refine
# out.png defaults to file.png in the current directory; a from:to is a --path query (an object by
# its `// label`, a variable as function::name).
set -euo pipefail

if [[ $# -lt 1 || $1 != *.cpp ]]; then
  echo "usage: $0 file.cpp [out.png] [ldc options...]" >&2
  exit 1
fi
src=$1
shift
png=$(basename "$src" .cpp).png
if [[ $# -gt 0 && $1 == *.png ]]; then
  png=$1
  shift
fi

args=()
previous=
for arg in "$@"; do
  if [[ $arg != -* && $arg == *:* && $previous != --* ]]; then
    args+=(--path "$arg")
  else
    args+=("$arg")
  fi
  previous=$arg
done

root=$(cd "$(dirname "$0")/.." && pwd)
llvm=${LLVM_BIN:-/usr/lib/llvm-22/bin}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$llvm/clang++" -S -g -fno-discard-value-names -Xclang -disable-O0-optnone -emit-llvm "$src" \
  -o "$work/raw.ll"
"$llvm/opt" -S -p=mem2reg "$work/raw.ll" -o "$work/prog.ll"
"$root/build/ldc" --pag-dot "$work/pag.dot" --pag-dot-code "$src" "${args[@]}" "$work/prog.ll" |
  sed -n '/^Refinement/p; /^Path/,/^Points-to/{/^Points-to/!p}'
c++filt < "$work/pag.dot" | dot -Tpng -o "$png"
echo "Wrote $png"
