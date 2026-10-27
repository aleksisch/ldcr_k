#!/usr/bin/env bash
# Install the official prebuilt development packages for Ubuntu 24.04 x86-64.
set -euo pipefail

if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
  echo "These packages require Linux x86-64 (tested on Ubuntu 24.04)." >&2
  exit 1
fi

deps=${1:-${LDCR_DEPS:-$HOME/.local/share/ldcr-deps}}
mkdir -p "$deps"
deps=$(cd "$deps" && pwd)
mkdir -p "$deps/.downloads"

fetch() {
  local url=$1 name=$2 checksum=$3
  local archive="$deps/.downloads/$name"
  if ! [[ -f "$archive" ]] || ! printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status; then
    curl --fail --location --retry 3 "$url" --output "$archive.part"
    printf '%s  %s\n' "$checksum" "$archive.part" | sha256sum --check
    mv "$archive.part" "$archive"
  fi
}

svf_release=https://github.com/SVF-tools/SVF/releases/download/SVF-3.3
fetch "$svf_release/SVF-3.3-ubuntu-24.04-x86_64.zip" \
  SVF-3.3-ubuntu-24.04-x86_64.zip \
  1515c0c95a0c566c809a07771ef51813eae5219d1299d0f063ec89a1a95d60a9
fetch "$svf_release/llvm-21.1.0-ubuntu22-rtti-x86-64.tar.gz" \
  llvm-21.1.0-ubuntu22-rtti-x86-64.tar.gz \
  3fc791d06760fd325f0ba9f2cb45ec5a1b2df1652a06a577751935edabd809f3
fetch https://github.com/Z3Prover/z3/releases/download/z3-4.15.4/z3-4.15.4-x64-glibc-2.39.zip \
  z3-4.15.4-x64-glibc-2.39.zip \
  a41b690e89c343931471506cdc6d957b6044a200fd2d240cc017432afdff7d3e

unzip -q -o "$deps/.downloads/SVF-3.3-ubuntu-24.04-x86_64.zip" 'SVF-linux-x86_64/*' -d "$deps"
mkdir -p "$deps/llvm-21.1.0"
tar -xzf "$deps/.downloads/llvm-21.1.0-ubuntu22-rtti-x86-64.tar.gz" \
  -C "$deps/llvm-21.1.0" --strip-components=1
unzip -q -o "$deps/.downloads/z3-4.15.4-x64-glibc-2.39.zip" -d "$deps"

# Emit a sourceable Bash environment, including runtime library paths.
{
  printf 'export SVF_DIR=%q\n' "$deps/SVF-linux-x86_64/lib/cmake/SVF"
  printf 'export LLVM_DIR=%q\n' "$deps/llvm-21.1.0/lib/cmake/llvm"
  printf 'export Z3_DIR=%q\n' "$deps/z3-4.15.4-x64-glibc-2.39"
  printf 'export PATH=%q:$PATH\n' "$deps/llvm-21.1.0/bin"
  printf 'export LD_LIBRARY_PATH=%q${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\n' \
    "$deps/SVF-linux-x86_64/lib:$deps/llvm-21.1.0/lib:$deps/z3-4.15.4-x64-glibc-2.39/bin"
} > "$deps/env.sh"
printf 'Dependencies installed. Run: source %q\n' "$deps/env.sh"
