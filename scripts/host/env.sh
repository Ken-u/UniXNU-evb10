# Build environment without root. Usage: TOOLS=... WS=<workspace> source scripts/host/env.sh
# TOOLS is a prebuilt UniXNU toolchain prefix (Python 3.12, LLVM, the xcrun/plutil shims and libbsd), made once by
# scripts/linux/setup-toolchain.py of the UniXNU checkout; it is shared, never rebuilt per workspace.
WS="${WS:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"; export WS
: "${TOOLS:?set TOOLS to the prebuilt toolchain prefix}"; export TOOLS
export UNIXNU_TOOLCHAIN=$TOOLS/toolchain
export UNIXNU_USR_BIN_SHIMS=$TOOLS/toolchain/shims
DEPS=$TOOLS/deps-root/usr
UUID=${UUID_PREFIX:-$WS/host/uuid-usr}
export PATH=$TOOLS/tools/python/bin:$TOOLS/tools/bin:$TOOLS/toolchain/shims:$TOOLS/toolchain/bin:$DEPS/bin:$PATH
export CPATH=$DEPS/include:$UUID/include
export LIBRARY_PATH=$DEPS/lib/x86_64-linux-gnu:$UUID/lib/x86_64-linux-gnu
export LD_LIBRARY_PATH=$DEPS/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LLVM_CONFIG=$TOOLS/tools/bin/llvm-config
export TMPDIR=$WS/tmp
