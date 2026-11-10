#!/usr/bin/env python3
"""Check the generated emitter's patch lookup, linking and unlinking paths."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
source = (cache / 'native-local/headless/emit_x64.cpp').read_text()
assert 'LocationDescriptorToFriendlyName(descriptor)' not in source
methods = '\n'.join(re.search(r'void EmitX64::' + name + r'\([^\n]*\) \{.*?^}',
                             source, re.M | re.S).group()
                    for name in ('Patch', 'Unpatch'))
program = r'''
#include <cassert>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
namespace IR { using LocationDescriptor = unsigned; }
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using CodePtr = const u8*;
struct EmitX64 {
    enum class PatchKind : u32 { Jg, Jz, Jmp, MovRcx };
    struct Code {
        u8 bytes[4096]{};
        CodePtr cursor = bytes + 999;
        unsigned moves = 0;
        CodePtr getCurr() const { return cursor; }
        CodePtr getCode() const { return bytes; }
        void SetCodePtr(CodePtr value) { cursor = value; ++moves; }
    } code;
    struct PatchInformation { std::array<u32, 2> first{}; std::unique_ptr<std::vector<u32>> rest; };
    std::map<IR::LocationDescriptor, PatchInformation> patch_information;
    std::map<CodePtr, CodePtr> targets;
    void EmitPatchJg(IR::LocationDescriptor, CodePtr target) { targets[code.cursor] = target; }
    void EmitPatchJz(IR::LocationDescriptor, CodePtr target) { targets[code.cursor] = target; }
    void EmitPatchJmp(IR::LocationDescriptor, CodePtr target) { targets[code.cursor] = target; }
    void EmitPatchMovRcx(CodePtr target) { targets[code.cursor] = target; }
    void Patch(const IR::LocationDescriptor&, CodePtr);
    void Unpatch(const IR::LocationDescriptor&);
};
METHODS
int main() {
    EmitX64 emitter;
    const CodePtr target = emitter.code.getCode() + 123;
    for (unsigned i = 0; i < 350000; ++i) emitter.Patch(i, target);
    assert(emitter.patch_information.empty());
    assert(emitter.code.moves == 0 && emitter.code.cursor == emitter.code.getCode() + 999);
    auto& patches = emitter.patch_information[7];
    patches.first = {{(10u << 2) | 0u, (20u << 2) | 1u}};
    patches.rest = std::make_unique<std::vector<u32>>(std::initializer_list<u32>{(30u << 2) | 2u, (40u << 2) | 3u, (50u << 2) | 0u});
    emitter.Patch(7, target);
    assert(emitter.targets.size() == 5 && emitter.code.cursor == emitter.code.getCode() + 999);
    for (const auto& [site, patched_target] : emitter.targets) assert(patched_target == target);
    emitter.Unpatch(7);
    for (const auto& [site, patched_target] : emitter.targets) assert(patched_target == nullptr);
    assert(emitter.code.cursor == emitter.code.getCode() + 999 && emitter.patch_information.size() == 1);
    // Retain incoming sites for recompilation/relinking.
    const CodePtr relinked_target = emitter.code.getCode() + 456;
    emitter.Patch(7, relinked_target);
    for (const auto& [site, patched_target] : emitter.targets) assert(patched_target == relinked_target);
    emitter.Unpatch(8);
    assert(emitter.patch_information.size() == 1 && emitter.code.cursor == emitter.code.getCode() + 999);
}
'''.replace('METHODS', methods.replace('nullptr', '0'))
with tempfile.TemporaryDirectory(prefix='eden-jit-patch-') as tmp:
    cpp = Path(tmp) / 'check.cpp'
    cpp.write_text(program)
    binary = Path(tmp) / 'check'
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: 350000 absent lookups create no records; all branch types link, unlink and relink')
