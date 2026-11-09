#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check active objects use the exact generated multiplayer declarations."""
import pathlib
import subprocess
import sys
HEADERS=('network/packet.h','network/room_member.h','core/internal_network/sockets.h',
         'core/internal_network/socket_proxy.h','core/hle/service/sockets/bsd.h')
def expected_path(build,header):
    return pathlib.Path(build).resolve()/'headless/multiplayer'/('socket-headers' if header.startswith('core/') else '')/header
def is_derived(build,header,path):
    p=pathlib.Path(path.replace('\\','/'))
    if not p.is_absolute():p=pathlib.Path(build).resolve()/p
    return p.resolve()==expected_path(build,header).resolve()
def main():
    build=pathlib.Path(sys.argv[1]).resolve()
    if (build/'multiplayer-hardening-off').exists():print('Multiplayer hardening off (Lichii base): header audit skipped');return
    def ninja(*args):return subprocess.run(['ninja','-C',str(build),'-t',*args],capture_output=True,text=True,check=True).stdout
    for header in HEADERS:
        p=expected_path(build,header)
        if not p.is_file() or p.is_symlink() or build not in p.resolve().parents:raise SystemExit('Missing or escaping generated header: '+header)
    active={line.split(': ',1)[0] for line in ninja('targets','all').splitlines()}
    seen=dict.fromkeys(HEADERS,0);wrong=[];target=None
    for line in ninja('deps').splitlines():
        if line and not line.startswith(' '):target=line.split(': ',1)[0];continue
        if target not in active:continue
        path=line.strip().replace('\\','/')
        for header in HEADERS:
            if path==header or path.endswith('/'+header):
                if is_derived(build,header,path):seen[header]+=1
                else:wrong.append(f'{target}: {path}')
    missing=[header for header,count in seen.items() if not count]
    if wrong or missing:
        print('\n'.join(wrong[:20]));raise SystemExit(f'Multiplayer headers: {len(wrong)} unapproved dependencies, missing {missing}')
    print(f'Multiplayer exact generated headers: {seen} PASS')
if __name__=='__main__':main()
