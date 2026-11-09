"""Adds the host-researched GSV (IGraphicsSupervisor) files to the native research overlay.

Source: E:/Dev/ProsperoCompoundBuild/night prototypes (tipc-request-probe header, frame-context
self_controller, bootstrap/syncpoint/native-memory headers, nvdrv/nvmap/SMMU changes).
Idempotent; records pinned/final hashes exactly as replay_native.py expects."""
import hashlib, json, pathlib, sys
root = pathlib.Path(__file__).resolve().parents[2]
bundle = root / 'tools/native-startup-research'
protos = pathlib.Path(sys.argv[1]); pinned = pathlib.Path(sys.argv[2])
sha = lambda b: hashlib.sha256(b).hexdigest()
P = 'src/core/hle/service/'
files = {
  P+'am/service/self_controller.h': 'graphics-supervisor-startup-probe/tipc-request-probe/after',
  P+'am/service/self_controller.cpp': 'gsv-frame-context-prototype/after',
  P+'am/service/research_graphics_bootstrap.h': 'graphics-bootstrap-prototype/after',
  P+'am/service/research_gsv_syncpoint_context.h': 'gsv-syncpoint-context-prototype/after',
  P+'am/service/research_syncpoint_pool.h': 'gsv-syncpoint-context-prototype/after',
  P+'am/service/research_native_memory.h': 'gsv-native-backing-prototype/after',
  P+'nvdrv/nvdrv_interface.h': 'gsv-nvdrv-session-prototype/after',
  P+'nvdrv/nvdrv_interface.cpp': 'gsv-nvdrv-session-prototype/after',
  P+'nvdrv/core/nvmap.h': 'gsv-native-backing-prototype/after',
  P+'nvdrv/core/nvmap.cpp': 'gsv-native-backing-prototype/after',
  P+'nvdrv/core/container.h': 'gsv-native-backing-prototype/after',
  'src/common/host_memory.h': 'smmu-physical-coverage-prototype/after',
  'src/common/sparse_large_vector.h': 'smmu-physical-coverage-prototype/after',
  'src/core/device_memory_manager.inc': 'smmu-physical-coverage-prototype/after',
}
m = json.loads((bundle/'manifest.json').read_text())
by = {f['relative']: f for f in m['files']}
for rel, src in files.items():
    data = (protos/src/rel).read_bytes()
    dst = bundle/'overlay/eden'/rel; dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(data)
    p = pinned/rel; ps = sha(p.read_bytes()) if p.exists() else None
    entry = by.get(rel)
    if entry is None:
        entry = dict(scope='eden', relative=rel, overlay='overlay/eden/'+rel, pinned_sha256=ps,
                     native_baseline_sha256=ps, final_sha256=None, native_baseline_absolute_path=None,
                     reviewed_before_sha256=[])
        m['files'].append(entry); by[rel] = entry
    if entry['final_sha256'] and entry['final_sha256'] != sha(data):
        entry['reviewed_before_sha256'] = sorted(set(entry['reviewed_before_sha256']) | {entry['final_sha256']})
    entry['final_sha256'] = sha(data); entry['gsv_research_source'] = src
# Enable the GSV endpoint in the native core build (fails closed without the remote-region profile).
cm = by['src/core/CMakeLists.txt']; cmf = bundle/cm['overlay']; text = cmf.read_text()
marker = 'PROSPERO_GRAPHICS_SUPERVISOR_PROBE'
if marker not in text:
    text += '\n# GSV research endpoint (ISelfController 2000); see tools/gsv-research.\ntarget_compile_definitions(core PRIVATE PROSPERO_GRAPHICS_SUPERVISOR_PROBE=1)\n'
    old = cm['final_sha256']; cmf.write_text(text)
    cm['reviewed_before_sha256'] = sorted(set(cm['reviewed_before_sha256']) | {old})
    cm['final_sha256'] = sha(cmf.read_bytes())
m['gsv_research'] = dict(added='2026-10-29', commands='ISelfController2000 -> GSV 4/11/101/111/112/114/115/201/202/1101/1102/1111/1112/1402',
                         note='1101/1102/1111/1112 follow firmware 0400000000000081 semantics; driver calls not emulated; no frame presentation.')
(bundle/'manifest.json').write_text(json.dumps(m, indent=2))
print('overlay files', len(m['files']))
