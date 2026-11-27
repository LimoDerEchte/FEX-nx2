#!/bin/bash
# SPDX-License-Identifier: MIT
# Run with Bash and the coreutils under test in PATH.
set -eu
export LC_ALL=C
# -d DIR makes the scratch folders in DIR (default: the working folder) and
# is passed on to the probe and the suite.
dir=.
if (( $# >= 2 )) && [[ $1 == -d ]]; then
    dir=$2
    shift 2
fi
if (( $# > 2 )); then
    printf 'Usage: %s [-d scratch-dir] [filesystem-abi-probe [fs-suite]]\n' "$0" >&2
    exit 2
fi
if (( $# >= 1 )); then
    abi_status=0
    "$1" -d "$dir" || abi_status=$?
    utility_status=0
    "$BASH" --noprofile --norc "$0" -d "$dir" || utility_status=$?
    suite_status=42
    if (( $# == 2 )); then
        suite_status=0
        "$2" -d "$dir" || suite_status=$?
    fi
    printf 'Filesystem acceptance results: ABI %d, coreutils %d, suite %d\n' "$abi_status" "$utility_status" "$suite_status"
    (( abi_status == 42 && utility_status == 0 && suite_status == 42 ))
    exit
fi
umask 077
scratch=$(mktemp -d "$dir/fex-coreutils.XXXXXX")
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
