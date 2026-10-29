#!/usr/bin/env python3
"""Make ../xnu/evb10-userland-mounts.patch (bsd/kern/kern_exec.c, two insertions),
../xnu/evb10-exit-trace.patch (bsd/kern/kern_exit.c, one insertion), ../xnu/evb10-mmap-grant.patch (bsd/kern/kern_mman.c, one insertion)
and ../xnu/evb10-thread-sample.patch (osfmk/kern/bsd_kern.c, appended block) from the patched XNU tree of a UniXNU build.

Why a generator and not the patch files alone: a unified diff needs exact line counts, and the C block
(evb10-userland-mounts.c.inc) is long and edited often. The generator takes the file as the tree has it,
takes OUR insertions out again if they are in (so it can be run on a built tree as well as on a fresh
one) and writes a diff against that.

  XNU_WORK=<UniXNU>/out/evb10/<os-profile>/evb10/tree/.cache/xnu-work ./make-patches.py

The tree has to be one where the ARM64, SoC and OS series are applied. The device series applies the
result afterwards, in the order of patches/xnu/series; no patch may touch the context of another
patch's hunk.
"""
import difflib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / 'xnu'
if 'XNU_WORK' not in os.environ:
    sys.exit('set XNU_WORK to the .cache/xnu-work directory of a UniXNU build tree')
TREE = Path(os.environ['XNU_WORK'])

HOOK_ANCHOR = '\tassert(path);\n'
HOOK = '''\tassert(path);
#if DEVELOPMENT || DEBUG
\t{
\t\t/* EVB10: the SystemOS cryptex and the Data volume, once, before the first exec of the init program. */
\t\textern int evb10_userland_prepare(proc_t, user_addr_t);
\t\t(void) evb10_userland_prepare(p, scratch_addr);
\t\t/* EVB10: evb10-exec=1 makes the owner's test program the first init program to try. */
\t\textern const char *evb10_init_override(const char *);
\t\tpath = evb10_init_override(path);
\t}
#endif
'''
FUNC_ANCHOR = '''
/*
 * load_return_to_errno
 *
'''

EXIT_ANCHOR = '\tproc_transend(p, 1);\n\tproc_unlock(p);\n\n\tproc_prepareexit(p, rv, perf_notify);\n'
EXIT_HOOK = '''\tproc_transend(p, 1);
\tproc_unlock(p);

#if DEVELOPMENT || DEBUG
\t{
\t\t/* EVB10: why a process went away (evb10-ps=N, patches/tools). */
\t\textern void evb10_exit_trace(proc_t, int);
\t\tevb10_exit_trace(p, rv);
\t}
#endif
\tproc_prepareexit(p, rv, perf_notify);
'''


MMAN_ANCHOR = '''\t\t\terror = mac_file_check_mmap(vfs_context_ucred(ctx),
\t\t\t    fp->fp_glob, prot, flags, file_pos + pageoff,
\t\t\t    &maxprot);
'''
MMAN_HOOK = MMAN_ANCHOR + '''#if DEVELOPMENT || DEBUG
\t\t\tif (error != 0) {
\t\t\t\t/* EVB10: the owner's directory of programs and libraries (evb10-exec=1, patches/tools). */
\t\t\t\textern int evb10_mmap_grant(vnode_t);
\t\t\t\tif (evb10_mmap_grant(vp)) {
\t\t\t\t\terror = 0;
\t\t\t\t}
\t\t\t}
#endif
'''


def pristine(relative, patch):
    """The tree's file without our patch: reverse-apply it to a copy if it is in, else the file as it is."""
    source = TREE / relative
    text = source.read_text()
    if not patch.exists():
        return text
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp) / 'file'
        copy.write_text(text)
        probe = subprocess.run(['patch', '-R', '-p1', '--dry-run', '-s', str(copy), str(patch)], capture_output=True, text=True)
        if probe.returncode != 0:
            return text
        subprocess.run(['patch', '-R', '-p1', '-s', str(copy), str(patch)], check=True)
        return copy.read_text()


def diff(relative, before, after, out):
    lines = difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile='a/' + relative, tofile='b/' + relative, n=3)
    out.write_text(''.join(lines))
    print('wrote', out)


def main():
    func = (HERE / 'evb10-userland-mounts.c.inc').read_text()
    patch = OUT / 'evb10-userland-mounts.patch'
    before = pristine('bsd/kern/kern_exec.c', patch)
    assert before.count(HOOK_ANCHOR) == 1, before.count(HOOK_ANCHOR)
    assert before.count(FUNC_ANCHOR) == 1, before.count(FUNC_ANCHOR)
    after = before.replace(HOOK_ANCHOR, HOOK, 1).replace(FUNC_ANCHOR, '\n' + func + FUNC_ANCHOR, 1)
    diff('bsd/kern/kern_exec.c', before, after, patch)

    patch = OUT / 'evb10-exit-trace.patch'
    before = pristine('bsd/kern/kern_exit.c', patch)
    assert before.count(EXIT_ANCHOR) == 1, before.count(EXIT_ANCHOR)
    after = before.replace(EXIT_ANCHOR, EXIT_HOOK, 1)
    diff('bsd/kern/kern_exit.c', before, after, patch)

    patch = OUT / 'evb10-mmap-grant.patch'
    before = pristine('bsd/kern/kern_mman.c', patch)
    assert before.count(MMAN_ANCHOR) == 1, before.count(MMAN_ANCHOR)
    after = before.replace(MMAN_ANCHOR, MMAN_HOOK, 1)
    diff('bsd/kern/kern_mman.c', before, after, patch)

    # osfmk/kern/bsd_kern.c: one block appended at the end of the file (the thread sampler of the ps table).
    sample = (HERE / 'evb10-thread-sample.c.inc').read_text()
    patch = OUT / 'evb10-thread-sample.patch'
    before = pristine('osfmk/kern/bsd_kern.c', patch)
    assert before.endswith('\n') and not before.endswith('\n\n'), 'bsd_kern.c should end with exactly one newline'
    diff('osfmk/kern/bsd_kern.c', before, before + '\n' + sample, patch)


if __name__ == '__main__':
    sys.exit(main())
