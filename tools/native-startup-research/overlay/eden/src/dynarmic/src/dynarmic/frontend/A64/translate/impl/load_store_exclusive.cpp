/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <optional>

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

// CAS/CASA/CASL/CASAL share the same 32-bit atomic old-value operation.
// The callback performs a full-barrier host CAS, conservatively stronger
// than each requested ordering. No exclusive status or load/store pair.
bool TranslatorVisitor::CAS32(bool /*acquire*/, Reg Rs, bool /*release*/, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) {
        address = SP(64);
    } else {
        address = X(64, Rn);
    }
    const IR::U32 expected = X(32, Rs);
    const IR::U32 desired = X(32, Rt);
    const IR::U32 observed = ir.AtomicCompareExchange32(address, expected, desired);
    X(32, Rs, observed); // SetW supplies architectural zero-extension; ZR discards.
    return true;
}

bool TranslatorVisitor::CAS16Orders(bool /*acquire*/, Reg Rs, bool /*release*/, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) {
        address = SP(64);
    } else {
        address = X(64, Rn);
    }
    const IR::U32 expected = X(32, Rs);
    const IR::U32 desired = X(32, Rt);
    const IR::U32 observed = ir.AtomicCompareExchange16(address, expected, desired);
    X(32, Rs, observed); // SetW supplies architectural zero-extension; ZR discards.
    return true;
}

bool TranslatorVisitor::CAS8Orders(bool /*acquire*/, Reg Rs, bool /*release*/, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) {
        address = SP(64);
    } else {
        address = X(64, Rn);
    }
    const IR::U32 expected = X(32, Rs);
    const IR::U32 desired = X(32, Rt);
    const IR::U32 observed = ir.AtomicCompareExchange8(address, expected, desired);
    X(32, Rs, observed); // SetW supplies architectural zero-extension; ZR discards.
    return true;
}

bool TranslatorVisitor::CAS64Orders(bool /*acquire*/, Reg Rs, bool /*release*/, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) {
        address = SP(64);
    } else {
        address = X(64, Rn);
    }
    const IR::U64 expected = X(64, Rs);
    const IR::U64 desired = X(64, Rt);
    const IR::U64 observed = ir.AtomicCompareExchange64(address, expected, desired);
    X(64, Rs, observed); // Preserve all 64 bits; XZR discards only the result.
    return true;
}


// The successful checked CAS is the single linearization point of this RMW.
// All four ordering forms conservatively use the callback's full barriers.
bool TranslatorVisitor::LDADD32Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchAdd32(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::SWP16Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicExchange16(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDEOR16Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchXor16(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDSET16Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchOr16(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDCLR16Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchClear16(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDADD16Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchAdd16(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::SWP8Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicExchange8(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDEOR8Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchXor8(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDSET8Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchOr8(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDCLR8Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchClear8(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDADD8Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchAdd8(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::SWP32Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicExchange32(address, operand);
    X(32, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDEOR32Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchXor32(address, operand);
    X(32, Rt, observed);
    return true;
}

// All four orders use the conservative full-barrier checked 64-bit CAS path.
bool TranslatorVisitor::LDADD64Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U64 operand = X(64, Rs); // Capture before a possibly aliased Rt write.
    const IR::U64 observed = ir.AtomicFetchAdd64(address, operand);
    X(64, Rt, observed);
    return true;
}

bool TranslatorVisitor::SWP64Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U64 operand = X(64, Rs); // Capture before a possibly aliased Rt write.
    const IR::U64 observed = ir.AtomicExchange64(address, operand);
    X(64, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDEOR64Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U64 operand = X(64, Rs); // Capture before a possibly aliased Rt write.
    const IR::U64 observed = ir.AtomicFetchXor64(address, operand);
    X(64, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDCLR64Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U64 operand = X(64, Rs); // Capture before a possibly aliased Rt write.
    const IR::U64 observed = ir.AtomicFetchClear64(address, operand);
    X(64, Rt, observed);
    return true;
}

bool TranslatorVisitor::LDSET64Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U64 operand = X(64, Rs); // Capture before a possibly aliased Rt write.
    const IR::U64 observed = ir.AtomicFetchOr64(address, operand);
    X(64, Rt, observed);
    return true;
}

// Atomic bit-clear: the checked full-barrier CAS loop returns the old word.
bool TranslatorVisitor::LDCLR32Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchClear32(address, operand);
    X(32, Rt, observed);
    return true;
}

// Atomic bit-set: the checked full-barrier CAS loop returns the old word.
bool TranslatorVisitor::LDSET32Orders(bool /*acquire*/, bool /*release*/, Reg Rs, Reg Rn, Reg Rt) {
    IR::U64 address;
    if (Rn == Reg::SP) address = SP(64);
    else address = X(64, Rn);
    const IR::U32 operand = X(32, Rs); // Capture before a possibly aliased Rt write.
    const IR::U32 observed = ir.AtomicFetchOr32(address, operand);
    X(32, Rt, observed);
    return true;
}

static bool ExclusiveSharedDecodeAndOperation(TranslatorVisitor& v, bool pair, size_t size, bool L, bool o0, std::optional<Reg> Rs, std::optional<Reg> Rt2, Reg Rn, Reg Rt) {
    // Shared Decode

    const auto acctype = o0 ? IR::AccType::ORDERED : IR::AccType::ATOMIC;
    const auto memop = L ? IR::MemOp::LOAD : IR::MemOp::STORE;
    const size_t elsize = 8 << size;
    const size_t regsize = elsize == 64 ? 64 : 32;
    const size_t datasize = pair ? elsize * 2 : elsize;

    // Operation

    const size_t dbytes = datasize / 8;

    if (memop == IR::MemOp::LOAD && pair && Rt == *Rt2) {
        return v.UnpredictableInstruction();
    } else if (memop == IR::MemOp::STORE && (*Rs == Rt || (pair && *Rs == *Rt2))) {
        if (!v.options.define_unpredictable_behaviour) {
            return v.UnpredictableInstruction();
        }
        // UNPREDICTABLE: The Constraint_NONE case is executed.
    } else if (memop == IR::MemOp::STORE && *Rs == Rn && Rn != Reg::R31) {
        return v.UnpredictableInstruction();
    }

    IR::U64 address;
    if (Rn == Reg::SP) {
        // TODO: Check SP Alignment
        address = v.SP(64);
    } else {
        address = v.X(64, Rn);
    }

    switch (memop) {
    case IR::MemOp::STORE: {
        IR::UAnyU128 data;
        if (pair && elsize == 64) {
            data = v.ir.Pack2x64To1x128(v.X(64, Rt), v.X(64, *Rt2));
        } else if (pair && elsize == 32) {
            data = v.ir.Pack2x32To1x64(v.X(32, Rt), v.X(32, *Rt2));
        } else {
            data = v.X(elsize, Rt);
        }
        const IR::U32 status = v.ExclusiveMem(address, dbytes, acctype, data);
        v.X(32, *Rs, status);
        break;
    }
    case IR::MemOp::LOAD: {
        const IR::UAnyU128 data = v.ExclusiveMem(address, dbytes, acctype);
        if (pair && elsize == 64) {
            v.X(64, Rt, v.ir.VectorGetElement(64, data, 0));
            v.X(64, *Rt2, v.ir.VectorGetElement(64, data, 1));
        } else if (pair && elsize == 32) {
            v.X(32, Rt, v.ir.LeastSignificantWord(data));
            v.X(32, *Rt2, v.ir.MostSignificantWord(data).result);
        } else {
            v.X(regsize, Rt, v.ZeroExtend(data, regsize));
        }
        break;
    }
    default:
        UNREACHABLE();
    }

    return true;
}

bool TranslatorVisitor::STXR(Imm<2> sz, Reg Rs, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, {}, Rn, Rt);
}

bool TranslatorVisitor::STLXR(Imm<2> sz, Reg Rs, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, {}, Rn, Rt);
}

bool TranslatorVisitor::STXP(Imm<1> sz, Reg Rs, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, Rt2, Rn, Rt);
}

bool TranslatorVisitor::STLXP(Imm<1> sz, Reg Rs, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, Rt2, Rn, Rt);
}

bool TranslatorVisitor::LDXR(Imm<2> sz, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, {}, Rn, Rt);
}

bool TranslatorVisitor::LDAXR(Imm<2> sz, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, {}, Rn, Rt);
}

bool TranslatorVisitor::LDXP(Imm<1> sz, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, Rt2, Rn, Rt);
}

bool TranslatorVisitor::LDAXP(Imm<1> sz, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, Rt2, Rn, Rt);
}

static bool OrderedSharedDecodeAndOperation(TranslatorVisitor& v, size_t size, bool L, bool o0, Reg Rn, Reg Rt) {
    // Shared Decode

    const auto acctype = !o0 ? IR::AccType::LIMITEDORDERED : IR::AccType::ORDERED;
    const auto memop = L ? IR::MemOp::LOAD : IR::MemOp::STORE;
    const size_t elsize = 8 << size;
    const size_t regsize = elsize == 64 ? 64 : 32;
    const size_t datasize = elsize;

    // Operation

    const size_t dbytes = datasize / 8;

    IR::U64 address;
    if (Rn == Reg::SP) {
        // TODO: Check SP Alignment
        address = v.SP(64);
    } else {
        address = v.X(64, Rn);
    }

    switch (memop) {
    case IR::MemOp::STORE: {
        const IR::UAny data = v.X(datasize, Rt);
        v.Mem(address, dbytes, acctype, data);
        break;
    }
    case IR::MemOp::LOAD: {
        const IR::UAny data = v.Mem(address, dbytes, acctype);
        v.X(regsize, Rt, v.ZeroExtend(data, regsize));
        break;
    }
    default:
        UNREACHABLE();
    }

    return true;
}

bool TranslatorVisitor::STLLR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 0;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

bool TranslatorVisitor::STLR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 1;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

bool TranslatorVisitor::LDLAR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 0;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

bool TranslatorVisitor::LDAR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 1;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

// RCsc acquire is a permitted stronger ordering than the RCpc load.
// Reuse the actual ordered read, address capture and width zero-extension.
bool TranslatorVisitor::LDAPR_reg(Imm<2> size, Reg Rn, Reg Rt) {
    return LDAR(size, Rn, Rt);
}

}  // namespace Dynarmic::A64
