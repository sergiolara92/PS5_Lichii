#!/usr/bin/env python3
# nup-check.py <NUP on the console> <loose folder> [package kind 0=base 1=update]: reads only the NUP's
# superblock and TOC over FTP (layout of PokeDinner's file_sys/nup_container.cpp) and compares the
# package's ExeFS/RomFS members (paths, sizes, stored SHA-256) with a loose dump on this PC.
import ftplib, hashlib, io, os, struct, sys

remote, loose = sys.argv[1], sys.argv[2]
kind = int(sys.argv[3]) if len(sys.argv) > 3 else 0
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=60); f.login(); f.voidcmd("TYPE I")


def read(offset, size):
    buf = io.BytesIO(); conn = f.transfercmd("RETR " + remote, rest=offset)
    while buf.tell() < size:
        c = conn.recv(min(1 << 20, size - buf.tell()))
        if not c: break
        buf.write(c)
    conn.close()
    try: f.voidresp()
    except ftplib.all_errors: pass
    return buf.getvalue()[:size]


sb = read(0, 0x80)
assert sb[:4] == b"NUP\x01", sb[:4]
toc_off, toc_size = struct.unpack_from("<QQ", sb, 8)
toc_sha = sb[0x18:0x38]
blob_off, file_size, pkg_count = struct.unpack_from("<QQI", sb, 0x38)
toc = read(toc_off, toc_size)
assert hashlib.sha256(toc).digest() == toc_sha, "TOC hash mismatch"
magic, pcount, dcount, fcount, pool_off, pool_size = struct.unpack_from("<IIQQQQ", toc, 0)
pos = 0x30; packages = []
for _ in range(pcount):
    tid, root, exefs, romfs, ver, k, flags = struct.unpack_from("<QIIIIBB", toc, pos); pos += 0x40
    packages.append(dict(title=tid, root=root, exefs=exefs, romfs=romfs, version=ver, kind=k, flags=flags))
name = lambda o, n: toc[pool_off + o:pool_off + o + n].decode("utf-8", "replace")
dirs = []
for _ in range(dcount):
    no, nl, first, count = struct.unpack_from("<IIII", toc, pos); pos += 0x18
    dirs.append((name(no, nl), first, count))
files = []
for _ in range(fcount):
    no, nl, size, eo, es = struct.unpack_from("<IIQQQ", toc, pos)
    sha = toc[pos + 0x20:pos + 0x40]; codec = toc[pos + 0x40]; pos += 0x50
    files.append((name(no, nl), size, eo, es, sha, codec))
children = struct.unpack_from(f"<{(pool_off - pos) // 4}I", toc, pos)
f.quit()
print("packages:", [(hex(p["title"]), p["kind"], p["version"], p["flags"]) for p in packages])


def walk(d, prefix=""):
    _, first, count = dirs[d]
    for c in children[first:first + count]:
        if c >= len(dirs): yield prefix + files[c - len(dirs)][0], files[c - len(dirs)]
        else: yield from walk(c, prefix + dirs[c][0] + "/")


pkg = next(p for p in packages if p["kind"] == kind)
bad = missing = checked = 0; codecs = {}
for part in ("exefs", "romfs"):
    members = dict(walk(pkg[part]))
    local = {os.path.relpath(os.path.join(b, n), os.path.join(loose, part)) for b, _, ns in os.walk(os.path.join(loose, part)) for n in ns if n != ".listing"}
    only_nup, only_local = set(members) - local, local - set(members)
    print(f"{part}: {len(members)} in the NUP, {len(local)} loose; only in NUP {len(only_nup)}, only loose {len(only_local)}",
          sorted(only_nup)[:3], sorted(only_local)[:3])
    for path, (nm, size, eo, es, sha, codec) in members.items():
        codecs[codec] = codecs.get(codec, 0) + 1
        lp = os.path.join(loose, part, path)
        if not os.path.exists(lp): missing += 1; continue
        h = hashlib.sha256()
        with open(lp, "rb") as s:
            while c := s.read(16 << 20): h.update(c)
        checked += 1
        if h.digest() != sha or os.path.getsize(lp) != size:
            bad += 1
            if bad <= 5: print("  differs:", part, path, size, os.path.getsize(lp), "codec", codec)
print(f"checked {checked}, differing {bad}, missing locally {missing}; codecs {codecs}")
