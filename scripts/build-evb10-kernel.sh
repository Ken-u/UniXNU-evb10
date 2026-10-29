#!/bin/bash
# Build the EVB10 kernel in the two passes the display archive needs.
#
#   scripts/build-evb10-kernel.sh            # pass A when needed, then pass B
#   DISPLAY_BUILD=0 scripts/build-evb10-kernel.sh   # only the display-less kernel
#
# Pass A builds configs/evb10_defconfig (no display). Pass B builds configs/evb10_display_defconfig: its
# archive step (scripts/build-evb10-display.py) reads the original IOMobileGraphicsFamily code from the
# kernel of pass A. Pass A is skipped when the previous kernel already carries that family.
#
# Settings come from .env (see .env.example): UNIXNU, UNIXNU_WRAP, UNIXNU_LLVM_OBJCOPY, JOBS.
set -euo pipefail

DEVICE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$DEVICE/.env" ] && set -a && . "$DEVICE/.env" && set +a
: "${UNIXNU:?set UNIXNU in .env to the UniXNU checkout}"
JOBS="${JOBS:-8}"
WRAP="${UNIXNU_WRAP:-}"
[ -n "${UNIXNU_LLVM_OBJCOPY:-}" ] && export UNIXNU_LLVM_OBJCOPY
DISPLAY_BUILD="${DISPLAY_BUILD:-1}"

unixnu() { (cd "$UNIXNU" && $WRAP ./unixnu "$@" -D "$DEVICE"); }
OUT="$UNIXNU/out/evb10/ipados-17.7.11-21H461/evb10"            # evb10_defconfig (pass A)
OUT_DISPLAY="$UNIXNU/out/evb10/ipados-17.7.11-21H461/evb10_display" # evb10_display_defconfig (pass B)

# Asm in patches and drivers: the target is Mach-O and the Darwin assembler treats ';' as a comment, so
# "dsb ishst; msr x, %0; isb" assembles to a lone dsb. Separate instructions with "\n\t".
semicolon='(__asm__|[^_[:alnum:]]asm)[[:space:]]*(volatile)?[[:space:]]*\([[:space:]]*"[^"]*;'
bad="$( { grep -nE "^\\+.*$semicolon" "$DEVICE"/patches/xnu/*.patch || true; \
          grep -nE "$semicolon" "$DEVICE"/patches/tools/*.inc || true; } )"
if [ -n "$bad" ]; then echo "$bad" >&2; echo "error: ';' inside inline assembly" >&2; exit 1; fi

build_pass() { # defconfig outdir
	echo "== configure $1"
	unixnu defconfig "$1"
	# make does not track the force-loaded archives: remove the linked kernel so the link always runs.
	rm -f "$2"/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf
	echo "== build kernel ($1)"
	unixnu build kernel -j "$JOBS"
}

PLAN="$OUT/kernel/kernel.json"
if [ "$DISPLAY_BUILD" = 1 ]; then
	if [ ! -f "$PLAN" ] || ! grep -q 'com.apple.iokit.IOMobileGraphicsFamily' "$PLAN"; then
		build_pass evb10_defconfig "$OUT"
	fi
	build_pass evb10_display_defconfig "$OUT_DISPLAY"
else
	build_pass evb10_defconfig "$OUT"
fi
if [ "$DISPLAY_BUILD" = 1 ]; then echo "kernel: $OUT_DISPLAY/kernel/kernel.macho"; else echo "kernel: $OUT/kernel/kernel.macho"; fi
