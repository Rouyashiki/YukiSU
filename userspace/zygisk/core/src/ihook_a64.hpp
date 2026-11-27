#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace yuki::ihook::a64 {

inline constexpr size_t kMaxStolen = 5;

enum class Error : uint8_t {
  None,
  InvalidInput,
  Unsupported,
  Overlap,
  Unreachable,
  Capacity,
  Mapping,
  Protect,
  PatchRejected,
  SymbolTooShort,
  InteriorBranch,
  ConcurrentUnsafe,
};

[[nodiscard]] inline const char *error_name(Error error) {
  switch (error) {
  case Error::None:
    return "none";
  case Error::InvalidInput:
    return "invalid_input";
  case Error::Unsupported:
    return "unsupported_instruction";
  case Error::Overlap:
    return "overlapping_literal";
  case Error::Unreachable:
    return "unreachable_branch";
  case Error::Capacity:
    return "insufficient_capacity";
  case Error::Mapping:
    return "no_trampoline_mapping";
  case Error::Protect:
    return "trampoline_protection";
  case Error::PatchRejected:
    return "patch_rejected";
  case Error::SymbolTooShort:
    return "symbol_too_short";
  case Error::InteriorBranch:
    return "interior_branch_target";
  case Error::ConcurrentUnsafe:
    return "concurrent_unsafe";
  }
  return "unknown";
}

enum class Kind : uint8_t {
  Copy,
  Adr,
  Adrp,
  Branch,
  Call,
  BranchUncond19,
  Conditional,
  Compare,
  Test,
  CompareBranch9,
  LiteralW,
  LiteralX,
  LiteralSigned,
  LiteralS,
  LiteralD,
  LiteralQ,
  Prefetch,
};

struct Instruction {
  uint32_t bits = 0;
  Kind kind = Kind::Copy;
  uintptr_t target = 0;
  uint8_t width = 0;
  bool writes_x16 = false;
  bool writes_x17 = false;
  bool is_terminal = false;
};

struct EmitPolicy {
  bool allow_indirect_far = false;
  bool target_bti_guarded = true;
};

struct RewriteIslands {
  uintptr_t branches[kMaxStolen] = {};
  uintptr_t resume = 0;
  bool branch_save_scratch[kMaxStolen] = {};
  bool resume_save_scratch = false;
};

struct Plan {
  Instruction instructions[kMaxStolen] = {};
  uint16_t offsets[kMaxStolen] = {};
  uint8_t lengths[kMaxStolen] = {};
  uint8_t count = 0;
  uint16_t code_size = 0;
  uint8_t covered_size = 0;
  uint8_t copied_size = 0;
  bool needs_continuation = true;
  bool absolute_continuation = false;
  uint8_t continuation_scratch = 16;
  RewriteIslands islands{};
  Error error = Error::None;
};

[[nodiscard]] inline bool is_bti_landing(uint32_t bits) {
  return bits == 0xd503245fU || // bti c
         bits == 0xd503249fU || // bti j
         bits == 0xd50324dfU;   // bti jc
}

[[nodiscard]] inline bool is_implicit_bti_pac(uint32_t bits) {
  return bits == 0xd503233fU || // paciasp
         bits == 0xd503237fU;   // pacibsp
}

[[nodiscard]] inline bool needs_entry_landing(uint32_t bits) {
  return is_bti_landing(bits) || is_implicit_bti_pac(bits);
}

[[nodiscard]] inline int64_t extend(uint32_t value, unsigned bits) {
  const uint32_t sign = uint32_t{1} << (bits - 1U);
  const uint32_t masked = value & ((uint32_t{1} << bits) - 1U);
  const uint32_t ext = (masked ^ sign) - sign;
  return (ext & 0x80000000U) != 0 ? -static_cast<int64_t>((~ext) + 1U)
                                  : static_cast<int64_t>(ext);
}

[[nodiscard]] inline bool add_signed(uintptr_t base, int64_t delta,
                                     uintptr_t *result) {
  if (result == nullptr)
    return false;
  if (delta >= 0) {
    const uint64_t mag = static_cast<uint64_t>(delta);
    if (static_cast<uint64_t>(base) > UINTPTR_MAX - mag)
      return false;
    *result = static_cast<uintptr_t>(static_cast<uint64_t>(base) + mag);
  } else {
    const uint64_t mag = static_cast<uint64_t>(-(delta + 1)) + 1ULL;
    if (static_cast<uint64_t>(base) < mag)
      return false;
    *result = static_cast<uintptr_t>(static_cast<uint64_t>(base) - mag);
  }
  return true;
}

[[nodiscard]] inline bool immediate(uintptr_t from, uintptr_t to, unsigned bits,
                                    int64_t *value) {
  if (value == nullptr || bits == 0 || bits > 26 || ((from | to) & 3U) != 0)
    return false;
  const uintptr_t limit = uintptr_t{1} << (bits + 1U);
  const bool backward = to < from;
  const uintptr_t distance = backward ? from - to : to - from;
  if (backward ? distance > limit : distance >= limit)
    return false;
  const int64_t delta = static_cast<int64_t>(distance / 4U);
  *value = backward ? -delta : delta;
  return true;
}

[[nodiscard]] inline bool adr_immediate(uintptr_t from, uintptr_t to,
                                        int64_t *value) {
  if (value == nullptr)
    return false;
  constexpr uintptr_t limit = uintptr_t{1} << 20;
  const bool backward = to < from;
  const uintptr_t distance = backward ? from - to : to - from;
  if (backward ? distance > limit : distance >= limit)
    return false;
  *value = backward ? -static_cast<int64_t>(distance)
                    : static_cast<int64_t>(distance);
  return true;
}

[[nodiscard]] inline bool adrp_immediate(uintptr_t from, uintptr_t to,
                                         int64_t *pages) {
  if (pages == nullptr)
    return false;
  const uintptr_t from_page = from & ~uintptr_t{4095};
  const uintptr_t to_page = to & ~uintptr_t{4095};
  constexpr uint64_t limit = uint64_t{1} << 20;
  const bool backward = to_page < from_page;
  const uint64_t distance_pages =
      static_cast<uint64_t>(backward ? from_page - to_page
                                     : to_page - from_page) >>
      12;
  if (backward ? distance_pages > limit : distance_pages >= limit)
    return false;
  *pages = backward ? -static_cast<int64_t>(distance_pages)
                    : static_cast<int64_t>(distance_pages);
  return true;
}

namespace detail {

[[nodiscard]] inline bool reg_is(uint32_t bits, unsigned shift, uint32_t reg) {
  return ((bits >> shift) & 31U) == reg;
}

[[nodiscard]] inline bool is_pauth_lr(uint32_t bits) {
  return bits == 0xd50324ffU ||                 // pacm
         bits == 0xdac1a3feU ||                 // paciasppc
         bits == 0xdac1a7feU ||                 // pacibsppc
         bits == 0xdac183feU ||                 // pacnbiasppc
         bits == 0xdac187feU ||                 // pacnbibsppc
         (bits & 0xffc0001fU) == 0xf380001fU || // autiasppc / autibsppc
         (bits & 0xffc0001fU) == 0x5500001fU || // retaasppc / retabsppc
         (bits & 0xfffffc1fU) == 0xdac1901eU || // autiasppcr
         (bits & 0xfffffc1fU) == 0xdac1941eU || // autibsppcr
         ((bits & 0xfffffbe0U) == 0xd65f0be0U &&
          (bits & 31U) != 31U); // ret*sppcr
}

[[nodiscard]] inline bool is_exclusive_mem(uint32_t bits) {
  return (bits & 0x3fa00000U) == 0x08000000U || // ldxr / stxr / ldaxr / stlxr
         (bits & 0xbfa00000U) == 0x88200000U;   // ldxp / stxp / ldaxp / stlxp
}

[[nodiscard]] inline bool is_supported_dp_imm(uint32_t bits) {
  if ((bits & 0xffc0001fU) == 0xf380001fU) // autiasppc / autibsppc
    return false;
  const unsigned op1 = (bits >> 23) & 7U;
  if (op1 <= 1U)
    return false; // PC-rel handled before this check.
  if (op1 == 5U) {
    const unsigned opc = (bits >> 29) & 3U;
    const bool sf = (bits & 0x80000000U) != 0;
    return opc != 1U && (sf || (bits & 0x00400000U) == 0);
  }
  return true;
}

[[nodiscard]] inline bool is_supported_dp_reg(uint32_t bits) {
  return !is_pauth_lr(bits);
}

[[nodiscard]] inline bool is_supported_load_store(uint32_t bits) {
  if ((bits & 0x3b000000U) == 0x18000000U)
    return false; // Literal loads handled before this check.
  return !is_exclusive_mem(bits);
}

[[nodiscard]] inline bool is_supported_branch_sys(uint32_t bits,
                                                  bool *is_terminal) {
  if ((bits & 0xfe000000U) == 0x54000000U || // B.cond / BC.cond
      (bits & 0x7c000000U) == 0x14000000U || // B / BL
      (bits & 0x7e000000U) == 0x34000000U || // CBZ / CBNZ
      (bits & 0x7e000000U) == 0x36000000U || // TBZ / TBNZ
      (bits & 0x7e000000U) == 0x74000000U || // FEAT_CMPBR
      (bits & 0xffc0001fU) == 0x5500001fU) { // RETAASPPC / RETABSPPC
    return false;
  }
  if ((bits & 0xff000000U) == 0xd4000000U) {
    return (bits & 0xffe0001fU) == 0xd4000001U; // SVC only
  }
  if ((bits & 0xffc00000U) == 0xd5000000U) {
    if (bits == 0xd50324ffU) // PACM
      return false;
    if ((bits & 0xfffff01fU) == 0xd503201fU) // HINT (includes BTI/PACIxSP/NOP)
      return true;
    if ((bits & 0xfffff0ffU) == 0xd503305fU || // CLREX
        (bits & 0xfffff0ffU) == 0xd503309fU || // DSB
        (bits & 0xfffff3ffU) == 0xd503323fU || // DSB nXS
        (bits & 0xfffff0ffU) == 0xd50330bfU || // DMB
        (bits & 0xfffff0ffU) == 0xd50330dfU) { // ISB
      return true;
    }
    return (bits & 0xfff00000U) == 0xd5300000U; // MRS
  }
  if ((bits & 0xfe000000U) == 0xd6000000U) {
    if (is_pauth_lr(bits))
      return false;
    if ((bits & 0xfffffc1fU) == 0xd61f0000U || // BR
        (bits & 0xfffffc1fU) == 0xd65f0000U || // RET
        (bits & 0xfefff800U) == 0xd61f0800U || // BRAA* / BRAB*
        (bits & 0xfffffbffU) == 0xd65f0bffU) { // RETAA / RETAB
      *is_terminal = true;
      return true;
    }
    return (bits & 0xfffffc1fU) == 0xd63f0000U || // BLR
           (bits & 0xfefff800U) == 0xd63f0800U;   // BLRAA* / BLRAB*
  }
  return false;
}

[[nodiscard]] inline bool writes_gpr(uint32_t bits, uint32_t reg) {
  const unsigned op0 = (bits >> 25) & 0xfU;
  if (op0 == 8U || op0 == 9U) {
    // DP immediate: Rd is [4:0] (for ADDS/SUBS/ANDS Rd=31 is XZR, never 16/17).
    return reg_is(bits, 0, reg);
  }
  if ((op0 & 7U) == 5U) {
    // DP register: Rd is [4:0].
    return reg_is(bits, 0, reg);
  }
  if ((op0 & 5U) == 4U) {
    // Loads and stores: conservative check on Rt[4:0], Rt2[14:10], Rs[20:16].
    if ((bits & 0x3b000000U) == 0x18000000U) {
      const bool vr = (bits & 0x04000000U) != 0;
      const unsigned opc = (bits >> 30) & 3U;
      return !vr && opc != 3U && reg_is(bits, 0, reg);
    }
    if ((bits & 0x04000000U) != 0) // FP/SIMD loads/stores write Vn, not GPRs.
      return false;
    // Pre/post-indexed load/store may writeback Rn[9:5].
    const bool may_write_rn = (bits & 0x3a000400U) == 0x38000400U ||
                              (bits & 0x3a400000U) == 0x28400000U;
    return reg_is(bits, 0, reg) || reg_is(bits, 10, reg) ||
           reg_is(bits, 16, reg) || (may_write_rn && reg_is(bits, 5, reg));
  }
  if ((op0 & 7U) == 7U) {
    // FP/SIMD DP: GPR destination only in FMOV/FCVT/SMOV/UMOV etc. when
    // bit 4..0 is Rd. Conservative: check bits [4:0] when top bits indicate GPR
    // transfer.
    return ((bits & 0x5f200000U) == 0x1e200000U ||
            (bits & 0x9f200000U) == 0x0e000000U) &&
           reg_is(bits, 0, reg);
  }
  if (op0 == 10U || op0 == 11U) {
    // MRS writes Rt[4:0]; PACIA1716/AUTIA1716 modify X17 only.
    if ((bits & 0xfff00000U) == 0xd5300000U)
      return reg_is(bits, 0, reg);
    if (bits == 0xd503211fU || bits == 0xd503215fU || bits == 0xd503219fU ||
        bits == 0xd50321dfU)
      return reg == 17U;
  }
  return false;
}

} // namespace detail

[[nodiscard]] inline Error decode(uint32_t bits, uintptr_t address,
                                  Instruction *out) {
  if (out == nullptr || (address & 3U) != 0)
    return Error::InvalidInput;
  *out = {bits,
          Kind::Copy,
          0,
          0,
          detail::writes_gpr(bits, 16),
          detail::writes_gpr(bits, 17),
          false};

  int64_t delta = 0;
  if ((bits & 0x1f000000U) == 0x10000000U) {
    const bool page = (bits & 0x80000000U) != 0;
    const uint32_t imm = (((bits >> 5) & 0x7ffffU) << 2) | ((bits >> 29) & 3U);
    delta = extend(imm, 21) * (page ? 4096 : 1);
    out->kind = page ? Kind::Adrp : Kind::Adr;
    return add_signed(page ? address & ~uintptr_t{4095} : address, delta,
                      &out->target)
               ? Error::None
               : Error::InvalidInput;
  }

  unsigned imm_bits = 0;
  unsigned shift = 5;
  if ((bits & 0x7c000000U) == 0x14000000U) {
    out->kind = (bits & 0x80000000U) != 0 ? Kind::Call : Kind::Branch;
    out->is_terminal = out->kind == Kind::Branch;
    imm_bits = 26;
    shift = 0;
  } else if ((bits & 0xff000000U) == 0x54000000U) {
    const unsigned cond = bits & 0xfU;
    out->kind = cond >= 14U ? Kind::BranchUncond19 : Kind::Conditional;
    out->is_terminal = cond >= 14U;
    imm_bits = 19;
  } else if ((bits & 0x7e000000U) == 0x34000000U) {
    out->kind = Kind::Compare;
    imm_bits = 19;
  } else if ((bits & 0x7e000000U) == 0x36000000U) {
    out->kind = Kind::Test;
    imm_bits = 14;
  } else if (((bits & 0x7f00c000U) == 0x74000000U ||
              (bits & 0xff008000U) == 0x74008000U ||
              (bits & 0x7f004000U) == 0x75000000U)) {
    const unsigned cc = (bits >> 21U) & 7U;
    if (cc == 4U || cc == 5U)
      return Error::Unsupported;
    out->kind = Kind::CompareBranch9;
    imm_bits = 9;
  } else if ((bits & 0x3b000000U) == 0x18000000U) {
    imm_bits = 19;
    switch ((bits >> 26) & 1U) {
    case 0:
      switch ((bits >> 30) & 3U) {
      case 0:
        out->kind = Kind::LiteralW;
        out->width = 4;
        break;
      case 1:
        out->kind = Kind::LiteralX;
        out->width = 8;
        break;
      case 2:
        out->kind = Kind::LiteralSigned;
        out->width = 4;
        break;
      case 3:
        out->kind = Kind::Prefetch;
        break;
      default:
        return Error::Unsupported;
      }
      break;
    case 1:
      switch ((bits >> 30) & 3U) {
      case 0:
        out->kind = Kind::LiteralS;
        out->width = 4;
        break;
      case 1:
        out->kind = Kind::LiteralD;
        out->width = 8;
        break;
      case 2:
        out->kind = Kind::LiteralQ;
        out->width = 16;
        break;
      default:
        return Error::Unsupported;
      }
      break;
    default:
      return Error::Unsupported;
    }
  } else {
    const unsigned op0 = (bits >> 25) & 0xfU;
    bool supported = false;
    if (op0 == 8U || op0 == 9U)
      supported = detail::is_supported_dp_imm(bits);
    else if ((op0 & 7U) == 5U)
      supported = detail::is_supported_dp_reg(bits);
    else if ((op0 & 5U) == 4U)
      supported = detail::is_supported_load_store(bits);
    else if ((op0 & 7U) == 7U)
      supported = true;
    else if (op0 == 10U || op0 == 11U)
      supported = detail::is_supported_branch_sys(bits, &out->is_terminal);
    return supported ? Error::None : Error::Unsupported;
  }

  const uint32_t mask = (uint32_t{1} << imm_bits) - 1U;
  delta = extend((bits >> shift) & mask, imm_bits) * 4;
  return add_signed(address, delta, &out->target) ? Error::None
                                                  : Error::InvalidInput;
}

[[nodiscard]] inline bool overlaps(uintptr_t a, size_t a_size, uintptr_t b,
                                   size_t b_size) {
  return a < b + b_size && b < a + a_size;
}

[[nodiscard]] inline bool is_literal(Kind kind) {
  return kind >= Kind::LiteralW;
}

[[nodiscard]] inline bool is_direct_branch(Kind kind) {
  return kind == Kind::Branch || kind == Kind::Call ||
         kind == Kind::BranchUncond19 || kind == Kind::Conditional ||
         kind == Kind::Compare || kind == Kind::Test ||
         kind == Kind::CompareBranch9;
}

[[nodiscard]] inline Error analyze(const uint8_t *source, size_t size,
                                   uintptr_t address, size_t covered_count,
                                   Plan *out) {
  if (source == nullptr || out == nullptr || covered_count == 0 ||
      covered_count > kMaxStolen || size < covered_count * 4U ||
      (address & 3U) != 0 || address > UINTPTR_MAX - covered_count * 4U)
    return Error::InvalidInput;
  *out = Plan{};
  const size_t covered_bytes = covered_count * 4U;
  out->count = static_cast<uint8_t>(covered_count);
  out->covered_size = static_cast<uint8_t>(covered_bytes);
  out->copied_size = static_cast<uint8_t>(covered_bytes);
  for (size_t i = 0; i != covered_count; ++i) {
    uint32_t bits = 0;
    memcpy(&bits, source + (i * 4U), sizeof(bits));
    const Error result =
        decode(bits, address + (i * 4U), &out->instructions[i]);
    if (result != Error::None)
      return out->error = result;
    const auto &insn = out->instructions[i];
    if (is_literal(insn.kind) && insn.width != 0 &&
        (insn.target > UINTPTR_MAX - insn.width ||
         overlaps(insn.target, insn.width, address, covered_bytes)))
      return out->error = Error::Overlap;
    if (insn.kind == Kind::Adr && insn.target >= address &&
        insn.target < address + covered_bytes)
      return out->error = Error::Overlap;
    if (is_direct_branch(insn.kind) && insn.target >= address &&
        insn.target < address + covered_bytes) {
      if (insn.kind == Kind::Call || ((insn.target - address) & 3U) != 0)
        return out->error = Error::Unsupported;
    }
  }
  out->needs_continuation = !out->instructions[covered_count - 1].is_terminal;
  return Error::None;
}

[[nodiscard]] inline Error analyze(const uint8_t *source, size_t size,
                                   uintptr_t address, Plan *out) {
  return analyze(source, size, address, 2, out);
}

[[nodiscard]] inline uintptr_t mapped_target(const Plan &plan, uintptr_t source,
                                             uintptr_t destination,
                                             uintptr_t target) {
  for (size_t i = 0; i != plan.count; ++i) {
    if (target == source + i * 4U)
      return destination + plan.offsets[i];
  }
  return target;
}

[[nodiscard]] inline uint32_t branch(uintptr_t from, uintptr_t to,
                                     bool call = false) {
  int64_t value = 0;
  if (!immediate(from, to, 26, &value))
    return 0;
  return (call ? 0x94000000U : 0x14000000U) |
         (static_cast<uint32_t>(value) & 0x03ffffffU);
}

inline void emit_word(uint8_t *out, size_t offset, uint32_t word) {
  memcpy(out + offset, &word, sizeof(word));
}

inline void emit_address(uint8_t *out, size_t offset, uint8_t rd,
                         uintptr_t target) {
  for (unsigned part = 0; part != 4; ++part) {
    const uint32_t op = part == 0 ? 0xd2800000U : 0xf2800000U;
    emit_word(
        out, offset + (static_cast<size_t>(part) * 4U),
        op | (part << 21) |
            (static_cast<uint32_t>(
                 (static_cast<uint64_t>(target) >> (part * 16U)) & 0xffffU)
             << 5) |
            rd);
  }
}

[[nodiscard]] inline size_t emit_entry_island(uint8_t *out, size_t capacity,
                                              uintptr_t trampoline_addr) {
  if (out == nullptr || capacity < 24U)
    return 0;
  emit_word(out, 0, 0xd50324dfU); // bti jc
  emit_address(out, 4, 16U, trampoline_addr);
  emit_word(out, 20, 0xd61f0200U); // br x16
  return 24U;
}

[[nodiscard]] inline size_t emit_branch_island(uint8_t *out, size_t capacity,
                                               uintptr_t island_addr,
                                               uintptr_t target, bool is_call,
                                               bool save_scratch = false) {
  if (out == nullptr || (island_addr & 3U) != 0)
    return 0;
  if (is_call || !save_scratch) {
    if (capacity < 8U)
      return 0;
    const uint32_t jmp = branch(island_addr + 4U, target);
    if (jmp == 0)
      return 0;
    emit_word(out, 0, 0xd50324dfU); // bti jc
    emit_word(out, 4, jmp);
    return 8U;
  }
  if (capacity < 12U)
    return 0;
  const uint32_t jmp = branch(island_addr + 8U, target);
  if (jmp == 0)
    return 0;
  emit_word(out, 0, 0xd50324dfU); // bti jc
  emit_word(out, 4, 0xa8c147f0U); // ldp x16, x17, [sp], #16
  emit_word(out, 8, jmp);
  return 12U;
}

[[nodiscard]] inline bool has_interior_branch(const uint8_t *code,
                                              size_t scan_bytes,
                                              uintptr_t base_addr,
                                              size_t covered_size) {
  if (code == nullptr || covered_size <= 4U || scan_bytes <= covered_size ||
      (base_addr & 3U) != 0)
    return false;
  const size_t limit = (scan_bytes < 4096U ? scan_bytes : 4096U) & ~size_t{3};
  struct LiteralSpan {
    uintptr_t begin = 0;
    uintptr_t end = 0;
  };
  LiteralSpan spans[32]{};
  size_t span_count = 0;
  uintptr_t furthest = base_addr + covered_size;
  for (size_t offset = 0; offset + 4U <= limit; offset += 4U) {
    const uintptr_t pc = base_addr + offset;
    bool in_literal = false;
    for (size_t s = 0; s != span_count; ++s) {
      if (pc >= spans[s].begin && pc < spans[s].end) {
        in_literal = true;
        break;
      }
    }
    if (in_literal)
      continue;
    uint32_t bits = 0;
    memcpy(&bits, code + offset, sizeof(bits));
    Instruction insn{};
    if (decode(bits, pc, &insn) == Error::None) {
      if (is_literal(insn.kind) && insn.width != 0 && insn.target > pc &&
          insn.target <= base_addr + limit - insn.width && span_count < 32U) {
        spans[span_count++] = {insn.target, insn.target + insn.width};
      }
      if (is_direct_branch(insn.kind)) {
        if (offset >= covered_size && insn.target > base_addr &&
            insn.target < base_addr + covered_size)
          return true;
        if (insn.kind != Kind::Call && insn.target > furthest &&
            insn.target < base_addr + limit)
          furthest = insn.target;
      }
      if (offset >= covered_size && insn.is_terminal && pc >= furthest)
        return false;
    } else if (offset >= covered_size &&
               (bits == 0x00000000U || (bits & 0xffe0001fU) == 0xd4200000U ||
                (bits & 0xffe0001fU) == 0xd4400000U) &&
               pc >= furthest) {
      return false;
    }
  }
  return false;
}

[[nodiscard]] inline bool has_interior_call(const uint8_t *code, size_t size) {
  for (size_t offset = 0; offset + 4U < size; offset += 4U) {
    uint32_t bits = 0;
    memcpy(&bits, code + offset, sizeof(bits));
    if ((bits & 0xfc000000U) == 0x94000000U ||
        (bits & 0xfffffc1fU) == 0xd63f0000U ||
        (bits & 0xfefff800U) == 0xd63f0800U)
      return true;
  }
  return false;
}

[[nodiscard]] inline bool resolve_thunk(const uint8_t *code, size_t available,
                                        uintptr_t address,
                                        uintptr_t *resolved) {
  if (code == nullptr || resolved == nullptr || (address & 3U) != 0 ||
      available < 4U)
    return false;
  size_t off = 0;
  uint32_t w0 = 0;
  memcpy(&w0, code, sizeof(w0));
  if (is_bti_landing(w0)) {
    off = 4U;
    if (available < 8U)
      return false;
    memcpy(&w0, code + off, sizeof(w0));
  }
  Instruction insn{};
  if (decode(w0, address + off, &insn) != Error::None)
    return false;
  if (insn.kind == Kind::Branch) {
    *resolved = insn.target;
    return true;
  }
  if (insn.kind == Kind::LiteralX && (w0 & 31U) == 16U &&
      available >= off + 8U) {
    uint32_t w1 = 0;
    memcpy(&w1, code + off + 4U, sizeof(w1));
    if (w1 == 0xd61f0200U && insn.target >= address &&
        insn.target <= address + available - 8U) {
      uintptr_t dst = 0;
      memcpy(&dst, code + (insn.target - address), sizeof(dst));
      if (dst != 0 && (dst & 3U) == 0) {
        *resolved = dst;
        return true;
      }
    }
  }
  if (insn.kind == Kind::Adrp && available >= off + 12U) {
    const uint32_t rd = w0 & 31U;
    if (rd != 16U && rd != 17U)
      return false;
    uint32_t w1 = 0;
    uint32_t w2 = 0;
    memcpy(&w1, code + off + 4U, sizeof(w1));
    memcpy(&w2, code + off + 8U, sizeof(w2));
    // ADD Xd, Xd, #imm12 (sh=0)
    const uint32_t add_expected = 0x91000000U | (rd << 5) | rd;
    const uint32_t br_expected = 0xd61f0000U | (rd << 5);
    if ((w1 & 0xffc003ffU) == add_expected && w2 == br_expected) {
      const uint32_t imm12 = (w1 >> 10U) & 0xfffU;
      *resolved = insn.target + imm12;
      return true;
    }
  }
  return false;
}

namespace detail {

[[nodiscard]] inline uint8_t select_far_scratch(const Plan &plan,
                                                size_t up_to_index) {
  bool clobbers_16 = false;
  bool clobbers_17 = false;
  for (size_t i = 0; i < up_to_index; ++i) {
    clobbers_16 |= plan.instructions[i].writes_x16;
    clobbers_17 |= plan.instructions[i].writes_x17;
  }
  if (!clobbers_16)
    return 16U;
  if (!clobbers_17)
    return 17U;
  return 31U;
}

} // namespace detail

[[nodiscard]] inline Error plan(const uint8_t *source, size_t size,
                                uintptr_t source_address, uintptr_t destination,
                                size_t capacity, size_t covered_count,
                                const EmitPolicy &policy,
                                const RewriteIslands &islands, Plan *out) {
  const Error analyzed =
      analyze(source, size, source_address, covered_count, out);
  if (analyzed != Error::None)
    return analyzed;
  if (capacity < 4 || destination > UINTPTR_MAX - capacity)
    return out->error = Error::Capacity;
  out->islands = islands;
  for (size_t i = 0; i != out->count; ++i) {
    out->offsets[i] = static_cast<uint16_t>(i * 4U);
    out->lengths[i] = 4;
  }

  for (unsigned pass = 0; pass != 8; ++pass) {
    bool changed = false;
    uint16_t cursor = 0;
    for (size_t i = 0; i != out->count; ++i) {
      out->offsets[i] = cursor;
      cursor = static_cast<uint16_t>(cursor + out->lengths[i]);
    }
    for (size_t i = 0; i != out->count; ++i) {
      const auto &insn = out->instructions[i];
      const uintptr_t at = destination + out->offsets[i];
      const uint8_t rd = insn.bits & 31U;
      int64_t value = 0;
      uint8_t length = 4;
      switch (insn.kind) {
      case Kind::Adr:
        if (rd == 31U)
          length = 4;
        else
          length = adr_immediate(at, insn.target, &value) ? 4 : 16;
        break;
      case Kind::Adrp:
        if (rd == 31U)
          length = 4;
        else
          length = adrp_immediate(at, insn.target, &value) ? 4 : 16;
        break;
      case Kind::Branch:
      case Kind::Call:
      case Kind::BranchUncond19: {
        const uintptr_t target =
            mapped_target(*out, source_address, destination, insn.target);
        if (!immediate(at, target, 26, &value)) {
          if (islands.branches[i] != 0) {
            if (insn.kind == Kind::Call) {
              if (detail::select_far_scratch(*out, i) == 31U)
                return out->error = Error::Unreachable;
              length = 20;
            } else {
              length = islands.branch_save_scratch[i] ? 24 : 20;
            }
          } else {
            if (!policy.allow_indirect_far || policy.target_bti_guarded)
              return out->error = Error::Unreachable;
            if (detail::select_far_scratch(*out, i) == 31U)
              return out->error = Error::Unreachable;
            length = 20;
          }
        }
        break;
      }
      case Kind::Conditional:
      case Kind::Compare:
      case Kind::Test: {
        const unsigned bits = insn.kind == Kind::Test ? 14U : 19U;
        const uintptr_t target =
            mapped_target(*out, source_address, destination, insn.target);
        if (!immediate(at, target, bits, &value)) {
          if (immediate(at + 4, target, 26, &value)) {
            length = 8;
          } else if (islands.branches[i] != 0) {
            length = islands.branch_save_scratch[i] ? 28 : 24;
          } else {
            if (!policy.allow_indirect_far || policy.target_bti_guarded ||
                detail::select_far_scratch(*out, i) == 31U)
              return out->error = Error::Unreachable;
            length = 24;
          }
        }
        break;
      }
      case Kind::CompareBranch9: {
        const uintptr_t target =
            mapped_target(*out, source_address, destination, insn.target);
        if (immediate(at, target, 9, &value)) {
          length = 4;
        } else if (immediate(at + 8, target, 26, &value)) {
          length = 12;
        } else if (islands.branches[i] != 0) {
          length = islands.branch_save_scratch[i] ? 32 : 28;
        } else {
          if (!policy.allow_indirect_far || policy.target_bti_guarded ||
              detail::select_far_scratch(*out, i) == 31U)
            return out->error = Error::Unreachable;
          length = 28;
        }
        break;
      }
      case Kind::LiteralW:
      case Kind::LiteralX:
      case Kind::LiteralSigned:
      case Kind::LiteralS:
      case Kind::LiteralD:
      case Kind::LiteralQ:
      case Kind::Prefetch:
        if (immediate(at, insn.target, 19, &value)) {
          length = 4;
        } else if (insn.kind <= Kind::LiteralSigned && rd != 31U) {
          length = 20;
        } else {
          length = detail::select_far_scratch(*out, i) != 31U ? 20 : 36;
        }
        break;
      case Kind::Copy:
        break;
      }
      if (length > out->lengths[i]) {
        out->lengths[i] = length;
        changed = true;
      }
    }
    if (!changed) {
      size_t continuation_size = 0;
      out->absolute_continuation = false;
      if (out->needs_continuation) {
        const uintptr_t tail_at = destination + cursor;
        const uintptr_t resume = source_address + out->covered_size;
        int64_t back = 0;
        if (immediate(tail_at, resume, 26, &back)) {
          continuation_size = 4;
        } else if (islands.resume != 0) {
          out->absolute_continuation = true;
          const uint8_t scratch = detail::select_far_scratch(*out, out->count);
          out->continuation_scratch = scratch != 31U ? scratch : 16U;
          continuation_size = islands.resume_save_scratch ? 24 : 20;
        } else {
          if (!policy.allow_indirect_far || policy.target_bti_guarded)
            return out->error = Error::Unreachable;
          const uint8_t scratch = detail::select_far_scratch(*out, out->count);
          if (scratch == 31U)
            return out->error = Error::Unreachable;
          out->absolute_continuation = true;
          out->continuation_scratch = scratch;
          continuation_size = 20;
        }
      }
      const size_t used = static_cast<size_t>(cursor) + continuation_size;
      if (used > capacity || used > UINT16_MAX)
        return out->error = Error::Capacity;
      out->code_size = static_cast<uint16_t>(used);
      return Error::None;
    }
  }
  return out->error = Error::Unreachable;
}

[[nodiscard]] inline Error plan(const uint8_t *source, size_t size,
                                uintptr_t source_address, uintptr_t destination,
                                size_t capacity, size_t covered_count,
                                const EmitPolicy &policy, Plan *out) {
  return plan(source, size, source_address, destination, capacity,
              covered_count, policy, RewriteIslands{}, out);
}

[[nodiscard]] inline Error plan(const uint8_t *source, size_t size,
                                uintptr_t source_address, uintptr_t destination,
                                size_t capacity, Plan *out) {
  return plan(source, size, source_address, destination, capacity, 2,
              EmitPolicy{}, RewriteIslands{}, out);
}

[[nodiscard]] inline Error emit(const Plan &plan, uintptr_t source,
                                uintptr_t destination, uint8_t *out,
                                size_t capacity) {
  if (plan.error != Error::None || out == nullptr || plan.count == 0 ||
      plan.code_size > capacity || plan.code_size < 4)
    return Error::Capacity;
  for (size_t i = 0; i != plan.count; ++i) {
    const auto &insn = plan.instructions[i];
    const size_t offset = plan.offsets[i];
    const uintptr_t at = destination + offset;
    const uint8_t length = plan.lengths[i];
    const uint8_t rd = insn.bits & 31U;
    int64_t imm = 0;
    uint32_t word = insn.bits;
    switch (insn.kind) {
    case Kind::Adr:
    case Kind::Adrp: {
      if (rd == 31U) {
        word = 0xd503201fU; // nop
        break;
      }
      if (length == 16) {
        emit_address(out, offset, rd, insn.target);
        continue;
      }
      const bool page = insn.kind == Kind::Adrp;
      int64_t delta = 0;
      const bool ok = page ? adrp_immediate(at, insn.target, &delta)
                           : adr_immediate(at, insn.target, &delta);
      if (!ok)
        return Error::Unreachable;
      const uint32_t encoded = static_cast<uint32_t>(delta) & 0x1fffffU;
      word =
          (word & 0x9f00001fU) | ((encoded & 3U) << 29) | ((encoded >> 2) << 5);
      break;
    }
    case Kind::Branch:
    case Kind::Call:
    case Kind::BranchUncond19: {
      const uintptr_t target =
          mapped_target(plan, source, destination, insn.target);
      if (length == 20) {
        const uint8_t scratch = detail::select_far_scratch(plan, i);
        if (scratch == 31U)
          return Error::Unreachable;
        const uintptr_t dest =
            plan.islands.branches[i] != 0 ? plan.islands.branches[i] : target;
        emit_address(out, offset, scratch, dest);
        emit_word(out, offset + 16U,
                  (insn.kind == Kind::Call ? 0xd63f0000U : 0xd61f0000U) |
                      (uint32_t{scratch} << 5));
        continue;
      }
      if (length == 24) {
        if (plan.islands.branches[i] == 0)
          return Error::Unreachable;
        emit_word(out, offset, 0xa9bf47f0U); // stp x16, x17, [sp, #-16]!
        emit_address(out, offset + 4U, 16U, plan.islands.branches[i]);
        emit_word(out, offset + 20U, 0xd61f0200U); // br x16
        continue;
      }
      word = branch(at, target, insn.kind == Kind::Call);
      if (word == 0)
        return Error::Unreachable;
      break;
    }
    case Kind::Conditional:
    case Kind::Compare:
    case Kind::Test: {
      const unsigned bits = insn.kind == Kind::Test ? 14U : 19U;
      const uint32_t mask = (uint32_t{1} << bits) - 1U;
      const uintptr_t target =
          mapped_target(plan, source, destination, insn.target);
      if (length == 8 || length == 24 || length == 28) {
        const uint32_t skip_words = length == 8 ? 2U : (length == 24 ? 6U : 7U);
        word ^= insn.kind == Kind::Conditional ? 1U : 0x01000000U;
        word = (word & ~(mask << 5)) | (skip_words << 5);
        emit_word(out, offset, word);
        if (length == 8) {
          const uint32_t jump = branch(at + 4, target);
          if (jump == 0)
            return Error::Unreachable;
          emit_word(out, offset + 4, jump);
        } else if (length == 28) {
          if (plan.islands.branches[i] == 0)
            return Error::Unreachable;
          emit_word(out, offset + 4U, 0xa9bf47f0U); // stp x16, x17, [sp, #-16]!
          emit_address(out, offset + 8U, 16U, plan.islands.branches[i]);
          emit_word(out, offset + 24U, 0xd61f0200U); // br x16
        } else {
          const uint8_t scratch = detail::select_far_scratch(plan, i);
          if (scratch == 31U)
            return Error::Unreachable;
          const uintptr_t dest =
              plan.islands.branches[i] != 0 ? plan.islands.branches[i] : target;
          emit_address(out, offset + 4U, scratch, dest);
          emit_word(out, offset + 20U, 0xd61f0000U | (uint32_t{scratch} << 5));
        }
        continue;
      }
      if (!immediate(at, target, bits, &imm))
        return Error::Unreachable;
      word = (word & ~(mask << 5)) | ((static_cast<uint32_t>(imm) & mask) << 5);
      break;
    }
    case Kind::CompareBranch9: {
      constexpr uint32_t mask = 0x1ffU;
      const uintptr_t target =
          mapped_target(plan, source, destination, insn.target);
      if (length == 4) {
        if (!immediate(at, target, 9, &imm))
          return Error::Unreachable;
        word =
            (word & ~(mask << 5)) | ((static_cast<uint32_t>(imm) & mask) << 5);
        break;
      }
      // Branch over skip jump to +8 when taken; skip stub at +4 when not taken.
      word = (word & ~(mask << 5)) | (uint32_t{2U} << 5);
      emit_word(out, offset, word);
      const uint32_t skip = branch(at + 4U, at + length);
      if (skip == 0)
        return Error::Unreachable;
      emit_word(out, offset + 4U, skip);
      if (length == 12) {
        const uint32_t jump = branch(at + 8U, target);
        if (jump == 0)
          return Error::Unreachable;
        emit_word(out, offset + 8U, jump);
      } else if (length == 32) {
        if (plan.islands.branches[i] == 0)
          return Error::Unreachable;
        emit_word(out, offset + 8U, 0xa9bf47f0U); // stp x16, x17, [sp, #-16]!
        emit_address(out, offset + 12U, 16U, plan.islands.branches[i]);
        emit_word(out, offset + 28U, 0xd61f0200U); // br x16
      } else {
        const uint8_t scratch = detail::select_far_scratch(plan, i);
        if (scratch == 31U)
          return Error::Unreachable;
        const uintptr_t dest =
            plan.islands.branches[i] != 0 ? plan.islands.branches[i] : target;
        emit_address(out, offset + 8U, scratch, dest);
        emit_word(out, offset + 24U, 0xd61f0000U | (uint32_t{scratch} << 5));
      }
      continue;
    }
    case Kind::LiteralW:
    case Kind::LiteralX:
    case Kind::LiteralSigned:
    case Kind::LiteralS:
    case Kind::LiteralD:
    case Kind::LiteralQ:
    case Kind::Prefetch: {
      if (length == 4) {
        if (!immediate(at, insn.target, 19, &imm))
          return Error::Unreachable;
        word = (word & ~0x00ffffe0U) |
               ((static_cast<uint32_t>(imm) & 0x7ffffU) << 5);
        break;
      }
      if (length == 20) {
        const uint32_t load = insn.kind == Kind::LiteralS        ? 0xbd400000U
                              : insn.kind == Kind::LiteralD      ? 0xfd400000U
                              : insn.kind == Kind::LiteralQ      ? 0x3dc00000U
                              : insn.kind == Kind::Prefetch      ? 0xf9800000U
                              : insn.kind == Kind::LiteralW      ? 0xb9400000U
                              : insn.kind == Kind::LiteralSigned ? 0xb9800000U
                                                                 : 0xf9400000U;
        if (insn.kind <= Kind::LiteralSigned && rd != 31U) {
          emit_address(out, offset, rd, insn.target);
          emit_word(out, offset + 16U, load | (rd << 5) | rd);
        } else {
          const uint8_t scratch = detail::select_far_scratch(plan, i);
          if (scratch == 31U)
            return Error::Unreachable;
          emit_address(out, offset, scratch, insn.target);
          emit_word(out, offset + 16U, load | (uint32_t{scratch} << 5) | rd);
        }
        continue;
      }
      // Reserve aligned stack storage for the temporary GPR.
      emit_word(out, offset, 0xd10043ffU);     // sub sp, sp, #16
      emit_word(out, offset + 4, 0xf90003f0U); // str x16, [sp]
      emit_address(out, offset + 8, 16, insn.target);
      const uint32_t load = insn.kind == Kind::LiteralS        ? 0xbd400000U
                            : insn.kind == Kind::LiteralD      ? 0xfd400000U
                            : insn.kind == Kind::LiteralQ      ? 0x3dc00000U
                            : insn.kind == Kind::Prefetch      ? 0xf9800000U
                            : insn.kind == Kind::LiteralW      ? 0xb9400000U
                            : insn.kind == Kind::LiteralSigned ? 0xb9800000U
                                                               : 0xf9400000U;
      emit_word(out, offset + 24U, load | (16U << 5) | rd);
      emit_word(out, offset + 28U, 0xf94003f0U); // ldr x16, [sp]
      emit_word(out, offset + 32U, 0x910043ffU); // add sp, sp, #16
      continue;
    }
    case Kind::Copy:
      break;
    }
    emit_word(out, offset, word);
  }
  if (plan.needs_continuation) {
    const uintptr_t resume = source + plan.covered_size;
    if (plan.absolute_continuation) {
      if (plan.islands.resume != 0 && plan.islands.resume_save_scratch) {
        const size_t tail = static_cast<size_t>(plan.code_size) - 24U;
        emit_word(out, tail, 0xa9bf47f0U); // stp x16, x17, [sp, #-16]!
        emit_address(out, tail + 4U, 16U, plan.islands.resume);
        emit_word(out, tail + 20U, 0xd61f0200U); // br x16
      } else {
        const uintptr_t dest =
            plan.islands.resume != 0 ? plan.islands.resume : resume;
        const size_t tail = static_cast<size_t>(plan.code_size) - 20U;
        emit_address(out, tail, plan.continuation_scratch, dest);
        emit_word(out, tail + 16U,
                  0xd61f0000U | (uint32_t{plan.continuation_scratch} << 5));
      }
    } else {
      const size_t tail = static_cast<size_t>(plan.code_size) - 4U;
      const uint32_t back = branch(destination + tail, resume);
      if (back == 0)
        return Error::Unreachable;
      emit_word(out, tail, back);
    }
  }
  return Error::None;
}

} // namespace yuki::ihook::a64
