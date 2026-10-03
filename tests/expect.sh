#!/bin/sh
# expect.sh <facts> <expected>: for every variable named in <expected>, its facts (-ldc-facts,
# contexts dropped) must be exactly the `var -> obj` lines listed there; `var ->` means none.
sed 's/\[ [^]]*\]//g' "$1" | sort -u > "$1.got"
grep -v -e '^#' -e ' ->$' "$2" | sort -u > "$1.want"
awk -F ' -> ' 'NR == FNR { if ($0 !~ /^#/) { sub(/ ->.*/, ""); want[$0] = 1 } next } $1 in want' \
    "$2" "$1.got" | diff "$1.want" -
