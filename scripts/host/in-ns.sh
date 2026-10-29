#!/bin/bash
# Run a command in a user namespace that overlays the xcrun/plutil/codesign shims on /usr/bin and csh/tcsh on /bin
# (the XNU build system hard-codes /usr/bin/xcrun and /bin/csh). Needed only on a machine without root.
set -e
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"
rm -rf "$WS/tmp/ovl-work" "$WS/tmp/ovl-work2" && mkdir -p "$WS/tmp/ovl-work" "$WS/tmp/ovl-work2"
export WS TOOLS
exec unshare -Urm bash -c '
  mount -t overlay overlay -o "lowerdir=/usr/bin,upperdir=$UNIXNU_USR_BIN_SHIMS,workdir=$WS/tmp/ovl-work" /usr/bin || exit 97
  mount -t overlay overlay -o "lowerdir=/bin,upperdir=$TOOLS/toolchain/shims-bin,workdir=$WS/tmp/ovl-work2" /bin || exit 98
  exec "$@"
' bash "$@"
