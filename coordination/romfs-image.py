#!/usr/bin/env python3
# romfs-image.py <romfs folder> <out.romfs> [--check N]: one RomFS image file from a loose RomFS
# folder, laid out exactly as Eden's RomFSBuildContext (core/file_sys/fsmitm_romfsbuild.cpp) builds
# it in memory, for a .eden descriptor's "romfs": {"kind": "image"}. A loose RomFS of tens of
# thousands of files exhausts the PS5's file and pthread resources; one image file does not.
# --check N: then looks N random files up through the image's hash tables (as Eden does) and
# compares their bytes with the folder's.
import os, random, struct, sys

ENTRY_EMPTY = 0xFFFFFFFF
PARTITION = 0x200


def align(v, a):
    return (v + a - 1) // a * a


def path_hash(parent, name):
    h = (parent ^ 123456789) & 0xFFFFFFFF
    for b in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= (b - 256 if b >= 128 else b) & 0xFFFFFFFF  # char is signed: sign-extended
    return h


def table_count(n):
    if n < 3: return 3
    if n < 19: return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)): n += 1
    return n


def build(src, out):
    dirs = {b"": {"parent": None, "name": b""}}   # path -> entry
    files = {}
    for base, subdirs, names in os.walk(src):
        rel = os.path.relpath(base, src)
        p = b"" if rel == "." else b"/" + os.fsencode(rel).replace(os.sep.encode(), b"/")
        for d in subdirs:
            dirs[p + b"/" + os.fsencode(d)] = {"parent": p, "name": os.fsencode(d)}
        for n in names:
            if n == ".listing": continue  # an FTP mirror's directory listing, not a game file
            full = os.path.join(base, n)
            files[p + b"/" + os.fsencode(n)] = {"parent": p, "name": os.fsencode(n), "src": full,
                                                "size": os.path.getsize(full)}
    fpaths, dpaths = sorted(files), sorted(dirs)
    # File data offsets and table offsets, in sorted order.
    part = eo = 0
    for fp in fpaths:
        f = files[fp]
        part = align(part, 16); f["offset"] = part; part += f["size"]
        f["entry"] = eo; eo += 0x20 + align(len(f["name"]), 4)
    file_table_size = eo
    eo = 0
    for dp in dpaths:
        d = dirs[dp]; d["entry"] = eo; eo += 0x18 + align(len(d["name"]), 4)
        d["child"] = d["file"] = d["sibling"] = None
    dir_table_size = eo
    for fp in reversed(fpaths):   # sibling = the parent's first file so far: list in sorted order
        f = files[fp]; par = dirs[f["parent"]]
        f["sibling"] = par["file"]; par["file"] = fp
    for dp in reversed(dpaths):
        if dp == b"": continue
        d = dirs[dp]; par = dirs[d["parent"]]
        d["sibling"] = par["child"]; par["child"] = dp
    fcount, dcount = table_count(len(fpaths)), table_count(len(dpaths))
    fhash, dhash = [ENTRY_EMPTY] * fcount, [ENTRY_EMPTY] * dcount
    ftable, dtable = bytearray(file_table_size), bytearray(dir_table_size)
    for fp in fpaths:
        f = files[fp]; pe = dirs[f["parent"]]["entry"]
        h = path_hash(pe, f["name"]) % fcount
        sib = files[f["sibling"]]["entry"] if f["sibling"] else ENTRY_EMPTY
        struct.pack_into("<IIQQII", ftable, f["entry"], pe, sib, f["offset"], f["size"], fhash[h], len(f["name"]))
        ftable[f["entry"] + 0x20:f["entry"] + 0x20 + len(f["name"])] = f["name"]
        fhash[h] = f["entry"]
    for dp in dpaths:
        d = dirs[dp]; pe = 0 if dp == b"" else dirs[d["parent"]]["entry"]
        h = path_hash(pe, d["name"]) % dcount
        sib = dirs[d["sibling"]]["entry"] if d["sibling"] else ENTRY_EMPTY
        child = dirs[d["child"]]["entry"] if d["child"] else ENTRY_EMPTY
        fil = files[d["file"]]["entry"] if d["file"] else ENTRY_EMPTY
        struct.pack_into("<IIIIII", dtable, d["entry"], pe, sib, child, fil, dhash[h], len(d["name"]))
        dtable[d["entry"] + 0x18:d["entry"] + 0x18 + len(d["name"])] = d["name"]
        dhash[h] = d["entry"]
    dir_hash_ofs = align(PARTITION + part, 4)
    header = struct.pack("<10Q", 0x50, dir_hash_ofs, 4 * dcount, dir_hash_ofs + 4 * dcount, dir_table_size,
                         dir_hash_ofs + 4 * dcount + dir_table_size, 4 * fcount,
                         dir_hash_ofs + 4 * dcount + dir_table_size + 4 * fcount, file_table_size, PARTITION)
    with open(out, "wb") as o:
        o.write(header); o.write(b"\0" * (PARTITION - len(header)))
        done = 0
        for i, fp in enumerate(fpaths):
            f = files[fp]
            o.seek(PARTITION + f["offset"])
            with open(f["src"], "rb") as s:
                while chunk := s.read(16 << 20): o.write(chunk)
            done += f["size"]
            if i % 10000 == 0: print(f"{i}/{len(fpaths)} files, {done / 2**30:.2f} GiB", flush=True)
        o.seek(dir_hash_ofs)
        o.write(struct.pack(f"<{dcount}I", *dhash)); o.write(dtable)
        o.write(struct.pack(f"<{fcount}I", *fhash)); o.write(ftable)
    print(f"{out}: {len(fpaths)} files, {len(dpaths)} directories, {os.path.getsize(out)} bytes")
    return fpaths, files


def lookup(img, path):
    # Eden-style: walk the directories by hash, then the file by hash.
    img.seek(0); h = struct.unpack("<10Q", img.read(0x50))
    dh_ofs, dh_size, dt_ofs, _, fh_ofs, fh_size, ft_ofs, _, part = h[1:]
    dcount, fcount = dh_size // 4, fh_size // 4
    def read_u32(o): img.seek(o); return struct.unpack("<I", img.read(4))[0]
    parts = path.strip(b"/").split(b"/")
    cur = 0
    for name in parts[:-1]:
        e = read_u32(dh_ofs + 4 * (path_hash(cur, name) % dcount))
        while e != ENTRY_EMPTY:
            img.seek(dt_ofs + e); p, _, _, _, nxt, nl = struct.unpack("<6I", img.read(0x18))
            if p == cur and img.read(nl) == name: break
            e = nxt
        assert e != ENTRY_EMPTY, (path, name)
        cur = e
    name = parts[-1]
    e = read_u32(fh_ofs + 4 * (path_hash(cur, name) % fcount))
    while e != ENTRY_EMPTY:
        img.seek(ft_ofs + e); p, _, off, size, nxt, nl = struct.unpack("<IIQQII", img.read(0x20))
        if p == cur and img.read(nl) == name:
            img.seek(part + off); return img.read(size)
        e = nxt
    raise AssertionError(path)


if __name__ == "__main__":
    src, out = sys.argv[1], sys.argv[2]
    fpaths, files = build(src, out)
    n = int(sys.argv[sys.argv.index("--check") + 1]) if "--check" in sys.argv else 0
    with open(out, "rb") as img:
        for fp in random.sample(fpaths, min(n, len(fpaths))) + fpaths[:1] + fpaths[-1:]:
            with open(files[fp]["src"], "rb") as s:
                assert lookup(img, fp) == s.read(), fp
    print(f"check: {n + 2} files found through the hash tables, bytes equal")
