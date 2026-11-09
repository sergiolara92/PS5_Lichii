#!/usr/bin/env python3
# fetch-save.py <console host> <title id> <destination>: copies a title's save folder from the
# console (/data/prosperoeden/user/nand/user/save/0000000000000000/<profile>/<title>) to the PC, for
# backups before tests touch it. The first profile holding the title is taken.
import ftplib
import os
import sys

host, title, dest = sys.argv[1], sys.argv[2].upper(), sys.argv[3]
f = ftplib.FTP()
f.connect(host, 2121, timeout=30)
f.login()
f.voidcmd('TYPE I')
base = '/data/prosperoeden/user/nand/user/save/0000000000000000'


def ls(path):
    out = []
    f.retrlines('LIST ' + path, out.append)
    return [(line.split(None, 8)[-1], line[0] == 'd') for line in out]


def get(remote, local):
    os.makedirs(local, exist_ok=True)
    for name, is_dir in ls(remote):
        if name in ('.', '..'):
            continue
        if is_dir:
            get(remote + '/' + name, local + '/' + name)
        else:
            with open(local + '/' + name, 'wb') as out:
                f.retrbinary('RETR ' + remote + '/' + name, out.write)


found = [base + '/' + profile + '/' + name
         for profile, is_dir in ls(base) if is_dir and profile not in ('.', '..')
         for name, _ in ls(base + '/' + profile) if name.upper() == title]
print('console saves:', [p.split('/')[-2] for p in found])
if found:
    get(found[0], dest)
f.quit()
