#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify every recorded sparse header is the exact generated build header.

Ninja dependencies may be build-relative or absolute. Resolve both against the
actual build directory; never classify an arbitrary '/headless/sparse/' suffix
as sufficient evidence of using the generated header.
"""
import hashlib
import pathlib
import subprocess
import sys

def is_derived(build, path):
    build=pathlib.Path(build).resolve()
    expected=build/'headless/sparse/common/sparse_large_vector.h'
    actual=pathlib.Path(path.replace('\\','/'))
    if not actual.is_absolute():actual=build/actual
    return actual.resolve()==expected.resolve()

def main():
    build=pathlib.Path(sys.argv[1]).resolve()
    expected=build/'headless/sparse/common/sparse_large_vector.h'
    if not expected.is_file() or expected.is_symlink() or build not in expected.resolve().parents:
        raise SystemExit('Missing or escaping generated sparse header')
    deps=subprocess.run(['ninja','-C',str(build),'-t','deps'],capture_output=True,text=True,check=True).stdout
    original,derived,target=[],0,None
    for line in deps.splitlines():
        if line and not line.startswith(' '):
            target=line.split(':',1)[0]
            continue
        path=line.strip()
        if not path.replace('\\','/').endswith('common/sparse_large_vector.h'):continue
        if is_derived(build,path):derived+=1
        else:original.append(f'{target}: {path}')
    if original or derived==0:
        for line in original[:20]:print('  saw an unapproved sparse header:',line)
        raise SystemExit(f'sparse header check failed: {len(original)} unapproved objects, {derived} generated')
    print(f'Sparse table header: {derived} objects compiled with exact generated header, none with another PASS')
    print('Generated sparse header SHA256:',hashlib.sha256(expected.read_bytes()).hexdigest())

if __name__=='__main__':main()
