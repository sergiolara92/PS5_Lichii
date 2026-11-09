#!/usr/bin/env python3
"""Hash-guarded final replay for a reviewed isolated native research candidate.
Default: check only. --apply is explicit. Active baseline paths are always refused.
The manifest describes every old native delta on a research-overlapping Eden file.
Unknown current bytes never trigger an archive reset, partial patch or fallback.
"""
import argparse, hashlib, json, os, tempfile
from pathlib import Path

def sha(data): return hashlib.sha256(data).hexdigest()
def safe_path(root, relative):
    rel=Path(relative)
    if rel.is_absolute() or '..' in rel.parts: raise RuntimeError('Unsafe relative path')
    path=(root/rel).resolve()
    if path!=root and root not in path.parents: raise RuntimeError('Path escapes target')
    return path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',required=True,type=Path)
    parser.add_argument('--port',required=True,type=Path)
    parser.add_argument('--scope',choices=['eden','port','all'],default='all')
    parser.add_argument('--manifest',type=Path,default=Path(__file__).resolve().with_name('manifest.json'))
    parser.add_argument('--apply',action='store_true')
    args=parser.parse_args()
    manifest=args.manifest.resolve(); bundle=manifest.parent
    data=json.loads(manifest.read_text())
    roots={'eden':args.source.resolve(),'port':args.port.resolve()}
    if args.apply:
        for active in data['forbidden_active_baseline_roots']:
            denied=Path(active).resolve()
            for root in roots.values():
                if root==denied or denied in root.parents:
                    raise RuntimeError('Refusing active native baseline root: '+str(root))
    # Preflight ALL files and overlay bytes before any mutation.
    entries=[]
    for item in data['files']:
        if args.scope!='all' and item['scope']!=args.scope: continue
        target=safe_path(roots[item['scope']],item['relative'])
        final_file=safe_path(bundle,item['overlay'])
        final=final_file.read_bytes()
        if sha(final)!=item['final_sha256']: raise RuntimeError('Overlay hash mismatch: '+str(final_file))
        if target.is_symlink(): raise RuntimeError('Refusing target symlink: '+str(target))
        if target.exists() and not target.is_file(): raise RuntimeError('Refusing non-file target: '+str(target))
        before=sha(target.read_bytes()) if target.is_file() else None
        accepted={item['pinned_sha256'],item['native_baseline_sha256'],item['final_sha256']}
        # Exact previous research states are explicitly reviewed, never path/metadata trust.
        for reviewed in item.get('reviewed_before_sha256', []):
            if not isinstance(reviewed,str) or len(reviewed)!=64 or any(c not in '0123456789abcdef' for c in reviewed):
                raise RuntimeError('Invalid reviewed-before digest')
            accepted.add(reviewed)
        if before not in accepted: raise RuntimeError('Unknown current bytes; refusing reset: '+str(target))
        entries.append((item,target,final,before))
    changed=sum(before!=item['final_sha256'] for item,target,final,before in entries)
    if args.apply:
        # Known complete final bytes make interruption replayable; unknown mutations fail closed.
        for item,target,final,before in entries:
            if before==item['final_sha256']: continue
            current=sha(target.read_bytes()) if target.is_file() else None
            if current!=before: raise RuntimeError('Target changed during replay: '+str(target))
            target.parent.mkdir(parents=True,exist_ok=True)
            fd,temp=tempfile.mkstemp(prefix='.native-replay-',dir=target.parent)
            try:
                with os.fdopen(fd,'wb') as stream:
                    stream.write(final); stream.flush(); os.fsync(stream.fileno())
                if target.exists(): os.chmod(temp,target.stat().st_mode & 0o777)
                elif item['relative'].endswith(('.py','.sh')): os.chmod(temp,0o755)
                else: os.chmod(temp,0o644)
                os.replace(temp,target)
            finally:
                if os.path.exists(temp): os.unlink(temp)
        for item,target,final,before in entries:
            if sha(target.read_bytes())!=item['final_sha256']: raise RuntimeError('Final hash mismatch: '+str(target))
    print(json.dumps({'scope':args.scope,'applied':args.apply,'files':len(entries),'changed':changed,
                      'all_expected_final':args.apply or changed==0}))

if __name__=='__main__':
    try: main()
    except Exception as error:
        raise SystemExit('NATIVE_REPLAY_REFUSED: '+str(error))