#!/bin/bash
# SPDX-License-Identifier: MIT
# Run with Bash and the coreutils under test in PATH.
set -eu
export LC_ALL=C
if (( $# > 2 )); then
    printf 'Usage: %s [filesystem-abi-probe [fs-suite]]\n' "$0" >&2
    exit 2
fi
if (( $# >= 1 )); then
    abi_status=0
    "$1" || abi_status=$?
    utility_status=0
    "$BASH" --noprofile --norc "$0" || utility_status=$?
    suite_status=42
    if (( $# == 2 )); then
        suite_status=0
        "$2" || suite_status=$?
    fi
    printf 'Filesystem acceptance results: ABI %d, coreutils %d, suite %d\n' "$abi_status" "$utility_status" "$suite_status"
    (( abi_status == 42 && utility_status == 0 && suite_status == 42 ))
    exit
fi
umask 077
scratch=$(mktemp -d ./fex-coreutils.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT

mkdir -p "$scratch/source/nested"
printf '%s\n' 'Switch 2 filesystem probe' > "$scratch/source/nested/file"
chmod 640 "$scratch/source/nested/file"
touch -t 200102030405.06 "$scratch/source/nested/file"
ln "$scratch/source/nested/file" "$scratch/source/nested/hard"
ln -s nested/file "$scratch/source/link"
cp -a "$scratch/source" "$scratch/copied"
mv "$scratch/copied" "$scratch/moved"

file="$scratch/moved/nested/file"
link="$scratch/moved/nested/hard"
[[ $(stat -c '%i' "$file") == "$(stat -c '%i' "$link")" ]]
[[ $(stat -c '%h %a %Y' "$file") == "$(stat -c '%h %a %Y' "$scratch/source/nested/file")" ]]
[[ $(readlink "$scratch/moved/link") == nested/file ]]
[[ $(cat "$scratch/moved/link") == 'Switch 2 filesystem probe' ]]
rm -- "$file"
[[ $(cat "$link") == 'Switch 2 filesystem probe' ]]
[[ $(stat -c '%h' "$link") == 1 ]]
ls -la "$scratch/moved"
df -P "$scratch"
rm -r -- "$scratch"
trap - EXIT
printf '%s\n' 'Coreutils filesystem probe passed.'
