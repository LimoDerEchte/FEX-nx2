#!/bin/bash
# SPDX-License-Identifier: MIT
# Run a staged console bundle's rootfs on the host kernel as a reference.
#
# Usage: RunPackagedNative.sh <bundle>/fex/rootfs [--root] <program> [arguments...]
#
# The rootfs is entered through user, mount and PID namespaces with a fresh
# /proc and a bound /dev/null, mirroring the guest namespace. Programs run as
# uid 1000 like the launcher's default user, or as root with --root (the
# equivalent of a "<program>.user" file containing "0 0"). Subordinate IDs
# from /etc/subuid let root switch to unrelated users inside the namespace.
set -eu
if (( $# < 2 )); then
    printf 'Usage: %s rootfs [--root] program [arguments...]\n' "$0" >&2
    exit 2
fi
rootfs=$(realpath -- "$1")
shift
user=1000
if [[ $1 == --root ]]; then
    user=0
    shift
fi
# Remove the mount points and scratch directory afterwards so the staged
# bundle stays identical to what is copied to the SD card.
created=()
for entry in proc dev scratch; do
    [[ -e $rootfs/$entry ]] || created+=("$rootfs/$entry")
done
cleanup() {
    for entry in "${created[@]}"; do
        rm -rf -- "$entry"
    done
}
trap cleanup EXIT
unshare --map-root-user --map-auto --mount --pid --fork --kill-child -- \
    /bin/bash -c '
        set -eu
        rootfs=$1 user=$2
        shift 2
        mkdir -p "$rootfs/proc" "$rootfs/dev"
        mount -t proc proc "$rootfs/proc"
        [[ -e $rootfs/dev/null ]] || touch "$rootfs/dev/null"
        mount --bind /dev/null "$rootfs/dev/null"
        # Scratch data lives in a writable directory owned by the test user.
        mkdir -p "$rootfs/scratch"
        chown "$user:$user" "$rootfs/scratch"
        cd "$rootfs"
        exec chroot --userspec="$user:$user" --groups="$user" "$rootfs" \
            /bin/bash --noprofile --norc -c "cd /scratch && exec \"\$@\"" bash "$@"
    ' bash "$rootfs" "$user" "$@"
