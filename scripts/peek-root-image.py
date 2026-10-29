#!/usr/bin/env python3
"""Look inside a `userdata-root.img` on the build machine, read-only, and say what the kernel will find.

    peek-root-image.py IMAGE [--expect NAME=APFS-VOLUME-UUID ...] [--hash]

Why it exists: the kernel picks the root volume by the APFS volume UUID in /chosen/root-matching
(board.toml `[payload.adt] root_match_volume_uuid`) and mounts the SystemOS cryptex by the
official UUID. If an image does not carry those volumes, the board spends 60 s waiting for a root
device and panics, and the only way to learn that is a whole board run. This reads the same
things the kernel's drivers will read -- the SHNGROOT manifest, the GPT of a window (if it has
one), the APFS checkpoint/omap/volume superblocks -- and prints them.

What it checks (all of it is what the image's own metadata says, nothing is mounted):
  * the manifest: magic, version, the SHA-256 of its first 4064 bytes, windows inside the image;
  * per window: a GPT (the IPSW `OS` disk image has one APFS partition) or a bare APFS container
    (the `SystemOS` cryptex has no partition map);
  * per APFS container: the latest checkpoint superblock, every volume's UUID/name/role and
    whether the volume is sealed -- the root-trust hook of the kernel only matters for an
    UNsealed root volume;
  * with --expect: every NAME=UUID must be found in that window, otherwise exit status 1;
  * with --hash: the SHA-256 of each window against the manifest (slow: reads the whole image).

Limits: only containers whose object map is a single leaf (one or two volumes) are walked; that
holds for the IPSW images this tree uses, and the script says so when it meets something bigger.
Exit status: 0 = fine, 1 = a check failed, 2 = usage / unreadable image.
"""
import argparse
import hashlib
import struct
import sys
import uuid

MANIFEST_BYTES = 4096
DIGEST_OFFSET = 4064
APFS_INCOMPAT_SEALED = 0x20


def fletcher64(buf):
    mod = 0xFFFFFFFF
    s1 = s2 = 0
    for w in struct.unpack("<%dI" % ((len(buf) - 8) // 4), buf[8:]):
        s1 = (s1 + w) % mod
        s2 = (s2 + s1) % mod
    c1 = mod - ((s1 + s2) % mod)
    c2 = mod - ((s1 + c1) % mod)
    return (c2 << 32) | c1


def checksum_ok(block):
    return struct.unpack_from("<Q", block, 0)[0] == fletcher64(block)


class Span:
    """A byte range of a file, read in blocks."""

    def __init__(self, fh, offset, size, block=4096):
        self.fh, self.offset, self.size, self.block = fh, offset, size, block

    def read(self, rel, length):
        if rel < 0 or rel + length > self.size:
            raise EOFError("read of %d bytes at +%d is outside the %d-byte span" % (length, rel, self.size))
        self.fh.seek(self.offset + rel)
        data = self.fh.read(length)
        if len(data) != length:
            raise EOFError("short read at +%d" % rel)
        return data

    def blk(self, n):
        return self.read(n * self.block, self.block)

    def sub(self, rel, size):
        return Span(self.fh, self.offset + rel, size, self.block)


def parse_manifest(fh, total):
    fh.seek(0)
    raw = fh.read(MANIFEST_BYTES)
    if len(raw) != MANIFEST_BYTES or raw[:8] != b"SHNGROOT":
        raise ValueError("no SHNGROOT manifest at the start of the image")
    version, count, image_bytes = struct.unpack_from("<IIQ", raw, 8)
    digest_ok = hashlib.sha256(raw[:DIGEST_OFFSET]).digest() == raw[DIGEST_OFFSET:DIGEST_OFFSET + 32]
    windows = []
    for i in range(count):
        base = 24 + 128 * i
        name = raw[base:base + 32].split(b"\0")[0].decode("ascii", "replace")
        offset, size, block_size, flags = struct.unpack_from("<QQII", raw, base + 32)
        sha = raw[base + 56:base + 88]
        windows.append(dict(name=name, offset=offset, size=size, block_size=block_size, flags=flags, sha256=sha))
    return dict(version=version, count=count, image_bytes=image_bytes, digest_ok=digest_ok, windows=windows)


def walk_gpt(span):
    """Return [(index, type_guid, start_byte, size_bytes)] of a GPT with 512-byte sectors, or None."""
    try:
        header = span.read(512, 512)
    except EOFError:
        return None
    if header[:8] != b"EFI PART":
        return None
    entries_lba, count, entry_size = struct.unpack_from("<QII", header, 72)
    table = span.read(entries_lba * 512, count * entry_size)
    parts = []
    for i in range(count):
        e = table[i * entry_size:(i + 1) * entry_size]
        if not any(e[:16]):
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        parts.append((i, uuid.UUID(bytes_le=e[:16]), first * 512, (last - first + 1) * 512))
    return parts


def walk_apfs(span, out):
    """Print and return [(uuid_upper, name, role, sealed)] of the volumes of the APFS container in span."""
    b0 = span.read(0, 4096)
    if b0[32:36] != b"NXSB":
        return None
    block_size = struct.unpack_from("<I", b0, 36)[0]
    span.block = block_size
    b0 = span.blk(0)
    count = struct.unpack_from("<Q", b0, 40)[0]
    out("    container: %d x %d-byte blocks (%.2f GiB), uuid %s" %
        (count, block_size, count * block_size / 2**30, str(uuid.UUID(bytes=b0[72:88])).upper()))
    desc_blocks, = struct.unpack_from("<I", b0, 104)
    desc_base, = struct.unpack_from("<Q", b0, 112)
    best = None
    for i in range(desc_blocks & 0x7FFFFFFF):
        b = span.blk(desc_base + i)
        if b[32:36] == b"NXSB" and checksum_ok(b):
            xid = struct.unpack_from("<Q", b, 16)[0]
            if best is None or xid > best[0]:
                best = (xid, b)
    if best is None:
        out("    !! no valid checkpoint superblock")
        return []
    xid, nx = best
    omap_oid, = struct.unpack_from("<Q", nx, 160)
    max_fs, = struct.unpack_from("<I", nx, 180)
    fs_oids = [o for o in (struct.unpack_from("<Q", nx, 184 + 8 * i)[0] for i in range(max_fs)) if o]
    omap = span.blk(omap_oid)
    root = span.blk(struct.unpack_from("<Q", omap, 48)[0])
    flags, level, nkeys = struct.unpack_from("<HHI", root, 32)
    if level != 0:
        out("    !! the object map is deeper than one leaf; volumes are not listed")
        return []
    toc_off, toc_len = struct.unpack_from("<HH", root, 40)
    key_base = 56 + toc_off + toc_len
    val_end = block_size - (40 if flags & 1 else 0)
    newest = {}
    for i in range(nkeys):
        k, v = struct.unpack_from("<HH", root, 56 + toc_off + 4 * i)
        koid, kxid = struct.unpack_from("<QQ", root, key_base + k)
        paddr = struct.unpack_from("<Q", root, val_end - v + 8)[0]
        if koid not in newest or kxid > newest[koid][0]:
            newest[koid] = (kxid, paddr)
    found = []
    out("    checkpoint xid %d, %d volume(s)" % (xid, len(fs_oids)))
    for oid in fs_oids:
        if oid not in newest:
            out("    !! volume oid %d is not in the object map" % oid)
            continue
        v = span.blk(newest[oid][1])
        if v[32:36] != b"APSB":
            out("    !! volume oid %d: block %d has no APSB magic" % (oid, newest[oid][1]))
            continue
        incompat, = struct.unpack_from("<Q", v, 56)
        vol_uuid = str(uuid.UUID(bytes=v[240:256])).upper()
        name = v[704:960].split(b"\0")[0].decode("utf-8", "replace")
        role, = struct.unpack_from("<H", v, 964)
        sealed = bool(incompat & APFS_INCOMPAT_SEALED)
        out("    volume %r  UUID %s  role 0x%x  %s  checksum %s" %
            (name, vol_uuid, role, "SEALED" if sealed else "unsealed", "ok" if checksum_ok(v) else "BAD"))
        found.append((vol_uuid, name, role, sealed))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--expect", action="append", default=[], metavar="NAME=UUID")
    ap.add_argument("--hash", action="store_true", help="also verify each window's SHA-256 (reads the whole image)")
    args = ap.parse_args()

    expect = {}
    for item in args.expect:
        name, sep, value = item.partition("=")
        if not sep or not name or not value:
            ap.error("--expect wants NAME=UUID, got %r" % item)
        expect[name] = value.strip().upper()

    try:
        fh = open(args.image, "rb")
    except OSError as exc:
        print("cannot open %s: %s" % (args.image, exc), file=sys.stderr)
        return 2
    fh.seek(0, 2)
    total = fh.tell()
    try:
        man = parse_manifest(fh, total)
    except ValueError as exc:
        print("%s: %s" % (args.image, exc), file=sys.stderr)
        return 2

    problems = []
    print("%s: %d bytes (%.2f GiB)" % (args.image, total, total / 2**30))
    print("manifest: version %d, %d window(s), image_bytes %d, digest %s" %
          (man["version"], man["count"], man["image_bytes"], "ok" if man["digest_ok"] else "BAD"))
    if not man["digest_ok"]:
        problems.append("manifest digest does not match")
    if man["image_bytes"] > total:
        problems.append("image_bytes %d is larger than the file (%d)" % (man["image_bytes"], total))

    for w in man["windows"]:
        print("window %-9s offset %d (0x%x)  size %d  block %d  %s" %
              (w["name"], w["offset"], w["offset"], w["size"], w["block_size"], "read-only" if w["flags"] & 1 else "writable"))
        if w["offset"] + w["size"] > total:
            problems.append("window %s runs past the end of the file" % w["name"])
            continue
        if args.hash:
            h = hashlib.sha256()
            fh.seek(w["offset"])
            left = w["size"]
            while left:
                chunk = fh.read(min(left, 8 << 20))
                if not chunk:
                    break
                h.update(chunk)
                left -= len(chunk)
            ok = h.digest() == w["sha256"]
            print("    sha256 %s" % ("ok" if ok else "MISMATCH"))
            if not ok:
                problems.append("window %s: sha256 mismatch" % w["name"])
        span = Span(fh, w["offset"], w["size"])
        containers = []
        parts = walk_gpt(span)
        if parts is None:
            print("    no partition map: looking for a bare APFS container")
            containers.append(span)
        else:
            for i, type_guid, start, size in parts:
                print("    GPT partition %d: type %s, %.2f GiB at +%d" % (i, type_guid, size / 2**30, start))
                if str(type_guid).lower() == "7c3457ef-0000-11aa-aa11-00306543ecac":
                    containers.append(span.sub(start, size))
        found = []
        for c in containers:
            vols = walk_apfs(c, print)
            if vols is None:
                print("    no APFS container here")
            else:
                found.extend(vols)
        if w["name"] in expect:
            want = expect.pop(w["name"])
            if any(v[0] == want for v in found):
                print("    expected volume %s: found" % want)
            else:
                print("    expected volume %s: NOT FOUND" % want)
                problems.append("window %s has no volume with UUID %s" % (w["name"], want))
    for name, value in expect.items():
        problems.append("--expect %s=%s: the image has no window of that name" % (name, value))

    for p in problems:
        print("PROBLEM: %s" % p, file=sys.stderr)
    print("result: %s" % ("fine" if not problems else "%d problem(s)" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
