#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace yuki::ihook::arm32 {

inline constexpr size_t kMaxInsns = 16;
inline constexpr size_t kMaxCoveredBytes = 32;

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
  // A32
  ArmBranch,
  ArmCall,
  ArmBlxImm,
  ArmBxPc,
  ArmAdr,
  ArmLiteral,
  ArmVldrLit,
  ArmAluPc,
  ArmLdrRegPc,
  // Thumb
  ThumbItGroup,
  ThumbItSingle,
  ThumbBranch16Cond,
  ThumbBranch16,
  ThumbCbz,
  ThumbBxPc,
  ThumbAdr16,
  ThumbLdrLit16,
  ThumbAddPc16,
  ThumbMovPc16,
  ThumbBranch32Cond,
  ThumbBranch32,
  ThumbBl32,
  ThumbBlx32,
  ThumbAdr32,
  ThumbLiteral32,
  ThumbPldPliLit32,
  ThumbVldrLit32,
  ThumbTb32,
};

struct Instruction {
  uint8_t offset = 0;
  uint8_t width = 0;
  Kind kind = Kind::Copy;
  uint32_t raw = 0;
  uint32_t raw_hi = 0;
  uint32_t target = 0;
  uint8_t access_width = 0;
  uint8_t it_cond = 0;
  uint8_t it_count = 0;
  uint8_t it_bytes[18] = {};
  uint8_t it_bytes_len = 0;
  Kind it_single_kind = Kind::Copy;
  bool is_terminal = false;
};

struct Plan {
  Instruction instructions[kMaxInsns] = {};
  uint16_t offsets[kMaxInsns] = {};
  uint8_t lengths[kMaxInsns] = {};
  uint8_t count = 0;
  uint16_t code_size = 0;
  uint8_t covered_size = 0;
  uint8_t copied_size = 0;
  bool thumb = false;
  bool needs_continuation = true;
  Error error = Error::None;
};

[[nodiscard]] inline int32_t sign_extend32(uint32_t value, unsigned bits) {
  const uint32_t sign = uint32_t{1} << (bits - 1U);
  const uint32_t masked = value & ((uint32_t{1} << bits) - 1U);
  const uint32_t ext = (masked ^ sign) - sign;
  return (ext & 0x80000000U) != 0 ? -static_cast<int32_t>((~ext) + 1U)
                                  : static_cast<int32_t>(ext);
}

[[nodiscard]] inline uint32_t align4_pc(uint32_t insn_addr, uint32_t bias) {
  return (insn_addr + bias) & ~uint32_t{3};
}

[[nodiscard]] inline bool overlaps(uint32_t a, size_t a_size, uint32_t b,
                                   size_t b_size) {
  return a_size != 0 && b_size != 0 && a <= UINT32_MAX - a_size &&
         b <= UINT32_MAX - b_size && a < b + b_size && b < a + a_size;
}

[[nodiscard]] inline uint32_t arm_expand_imm(uint32_t imm12) {
  const uint32_t imm = imm12 & 0xffU;
  const unsigned rot = ((imm12 >> 8U) & 0xfU) * 2U;
  return rot == 0 ? imm : ((imm >> rot) | (imm << (32U - rot)));
}

[[nodiscard]] inline bool a32_b_reachable(uint32_t from, uint32_t to) {
  if (from > 0xfffffff7U || ((from | to) & 3U) != 0)
    return false;
  const uint32_t pc = from + 8U;
  return to >= pc ? (to - pc <= 33554428U) : (pc - to <= 33554432U);
}

[[nodiscard]] inline bool t32_b_reachable(uint32_t from, uint32_t to_untagged) {
  if (from > 0xfffffffbU || ((from | to_untagged) & 1U) != 0)
    return false;
  const uint32_t pc = from + 4U;
  return to_untagged >= pc ? (to_untagged - pc <= 16777214U)
                           : (pc - to_untagged <= 16777216U);
}

[[nodiscard]] inline bool t32_blx_reachable(uint32_t from,
                                            uint32_t to_untagged) {
  if (from > 0xfffffffbU || (from & 1U) != 0 || (to_untagged & 3U) != 0)
    return false;
  const uint32_t pc = align4_pc(from, 4U);
  return to_untagged >= pc ? (to_untagged - pc <= 16777212U)
                           : (pc - to_untagged <= 16777216U);
}

[[nodiscard]] inline bool t32_b_cond_reachable(uint32_t from,
                                               uint32_t to_untagged) {
  if (from > 0xfffffffbU || ((from | to_untagged) & 1U) != 0)
    return false;
  const uint32_t pc = from + 4U;
  return to_untagged >= pc ? (to_untagged - pc <= 1048574U)
                           : (pc - to_untagged <= 1048576U);
}

[[nodiscard]] inline bool t16_b_cond_reachable(uint32_t from,
                                               uint32_t to_untagged) {
  if (from > 0xfffffffbU || ((from | to_untagged) & 1U) != 0)
    return false;
  const uint32_t pc = from + 4U;
  return to_untagged >= pc ? (to_untagged - pc <= 254U)
                           : (pc - to_untagged <= 256U);
}

[[nodiscard]] inline bool t16_b_reachable(uint32_t from, uint32_t to_untagged) {
  if (from > 0xfffffffbU || ((from | to_untagged) & 1U) != 0)
    return false;
  const uint32_t pc = from + 4U;
  return to_untagged >= pc ? (to_untagged - pc <= 2046U)
                           : (pc - to_untagged <= 2048U);
}

[[nodiscard]] inline bool t16_cbz_reachable(uint32_t from,
                                            uint32_t to_untagged) {
  if (from > 0xfffffffbU || ((from | to_untagged) & 1U) != 0)
    return false;
  const uint32_t pc = from + 4U;
  return to_untagged >= pc && (to_untagged - pc <= 126U);
}

[[nodiscard]] inline bool thumb_is_32bit(uint16_t instruction) {
  const uint16_t prefix = instruction >> 11U;
  return prefix == 0x1dU || prefix == 0x1eU || prefix == 0x1fU;
}

[[nodiscard]] inline bool thumb_is_it(uint16_t hw) {
  return (hw & 0xff00U) == 0xbf00U && (hw & 0xfU) != 0U;
}

[[nodiscard]] inline unsigned thumb_it_count(uint16_t hw) {
  const unsigned mask = hw & 0xfU;
  if ((mask & 1U) != 0U)
    return 4U;
  if ((mask & 2U) != 0U)
    return 3U;
  if ((mask & 4U) != 0U)
    return 2U;
  return 1U;
}

[[nodiscard]] inline bool thumb_inside_it(uintptr_t address, size_t page_size) {
  if (page_size == 0 || (address & 1U) != 0)
    return true;
  const size_t page_offset = static_cast<size_t>(address % page_size);
  if (page_offset == 0)
    return false;
  const size_t max_back = page_offset < 18U ? page_offset : 18U;
  for (size_t back = 2U; back <= max_back; back += 2U) {
    uint16_t hw = 0;
    memcpy(&hw, reinterpret_cast<const void *>(address - back), sizeof(hw));
    if (!thumb_is_it(hw) || (hw & 0x00f0U) == 0x00f0U)
      continue;
    size_t prefix_run = 0;
    const size_t avail_before = page_offset - back;
    for (size_t off = 2U; off <= avail_before && prefix_run < 32U; off += 2U) {
      uint16_t prev = 0;
      memcpy(&prev, reinterpret_cast<const void *>(address - back - off),
             sizeof(prev));
      if (!thumb_is_32bit(prev))
        break;
      ++prefix_run;
    }
    if ((prefix_run & 1U) != 0U)
      continue;
    const unsigned count = thumb_it_count(hw);
    size_t cursor = 2U;
    bool valid = true;
    for (unsigned i = 0; i < count; ++i) {
      if (back <= cursor)
        return true;
      uint16_t next_hw = 0;
      memcpy(&next_hw, reinterpret_cast<const void *>(address - back + cursor),
             sizeof(next_hw));
      cursor += thumb_is_32bit(next_hw) ? 4U : 2U;
      if (cursor > 18U) {
        valid = false;
        break;
      }
    }
    if (valid && cursor > back)
      return true;
  }
  return false;
}

namespace detail {

inline void write_u16(uint8_t *out, size_t offset, uint16_t v) {
  memcpy(out + offset, &v, sizeof(v));
}

inline void write_u32(uint8_t *out, size_t offset, uint32_t v) {
  memcpy(out + offset, &v, sizeof(v));
}

inline void write_t32(uint8_t *out, size_t offset, uint16_t hw1, uint16_t hw2) {
  write_u16(out, offset, hw1);
  write_u16(out, offset + 2U, hw2);
}

[[nodiscard]] inline bool encode_t32_b_t4(uint32_t from, uint32_t to_untagged,
                                          uint16_t *hw1, uint16_t *hw2) {
  if (!t32_b_reachable(from, to_untagged))
    return false;
  const uint32_t imm = to_untagged - (from + 4U);
  const uint32_t s = (imm >> 24U) & 1U;
  const uint32_t i1 = (imm >> 23U) & 1U;
  const uint32_t i2 = (imm >> 22U) & 1U;
  const uint32_t j1 = ((~i1) & 1U) ^ s;
  const uint32_t j2 = ((~i2) & 1U) ^ s;
  *hw1 = static_cast<uint16_t>(0xf000U | (s << 10U) | ((imm >> 12U) & 0x3ffU));
  *hw2 = static_cast<uint16_t>(0x9000U | (j1 << 13U) | (j2 << 11U) |
                               ((imm >> 1U) & 0x7ffU));
  return true;
}

[[nodiscard]] inline bool encode_t32_bl_t1(uint32_t from, uint32_t to_untagged,
                                           uint16_t *hw1, uint16_t *hw2) {
  if (!encode_t32_b_t4(from, to_untagged, hw1, hw2))
    return false;
  *hw2 |= 0x4000U; // bit 14 = 1 -> BL
  return true;
}

[[nodiscard]] inline bool encode_t32_blx_t2(uint32_t from, uint32_t to_untagged,
                                            uint16_t *hw1, uint16_t *hw2) {
  if (!t32_blx_reachable(from, to_untagged))
    return false;
  const uint32_t imm = to_untagged - align4_pc(from, 4U);
  const uint32_t s = (imm >> 24U) & 1U;
  const uint32_t i1 = (imm >> 23U) & 1U;
  const uint32_t i2 = (imm >> 22U) & 1U;
  const uint32_t j1 = ((~i1) & 1U) ^ s;
  const uint32_t j2 = ((~i2) & 1U) ^ s;
  *hw1 = static_cast<uint16_t>(0xf000U | (s << 10U) | ((imm >> 12U) & 0x3ffU));
  *hw2 = static_cast<uint16_t>(0xc000U | (j1 << 13U) | (j2 << 11U) |
                               ((imm >> 1U) & 0x7feU));
  return true;
}

[[nodiscard]] inline bool encode_t32_b_t3(uint32_t from, uint32_t to_untagged,
                                          uint8_t cond, uint16_t *hw1,
                                          uint16_t *hw2) {
  if (cond >= 14U || !t32_b_cond_reachable(from, to_untagged))
    return false;
  const uint32_t imm = to_untagged - (from + 4U);
  const uint32_t s = (imm >> 20U) & 1U;
  const uint32_t j2 = (imm >> 19U) & 1U;
  const uint32_t j1 = (imm >> 18U) & 1U;
  *hw1 = static_cast<uint16_t>(0xf000U | (s << 10U) | (uint32_t{cond} << 6U) |
                               ((imm >> 12U) & 0x3fU));
  *hw2 = static_cast<uint16_t>(0x8000U | (j1 << 13U) | (j2 << 11U) |
                               ((imm >> 1U) & 0x7ffU));
  return true;
}

[[nodiscard]] inline bool encode_a32_branch(uint32_t from, uint32_t to,
                                            uint8_t cond, bool link,
                                            uint32_t *out) {
  if (cond == 15U || !a32_b_reachable(from, to))
    return false;
  const uint32_t imm = (to - (from + 8U)) >> 2U;
  *out = (uint32_t{cond} << 28U) | (link ? 0x0b000000U : 0x0a000000U) |
         (imm & 0x00ffffffU);
  return true;
}

[[nodiscard]] inline bool encode_a32_blx(uint32_t from, uint32_t to_untagged,
                                         uint32_t *out) {
  if (((from & 3U) != 0) || ((to_untagged & 1U) != 0))
    return false;
  if (from > 0xfffffff7U)
    return false;
  const uint32_t pc = from + 8U;
  const bool in_range = to_untagged >= pc ? (to_untagged - pc <= 33554430U)
                                          : (pc - to_untagged <= 33554432U);
  if (!in_range)
    return false;
  const uint32_t imm = to_untagged - pc;
  const uint32_t h = (imm >> 1U) & 1U;
  *out = 0xfa000000U | (h << 24U) | ((imm >> 2U) & 0x00ffffffU);
  return true;
}

[[nodiscard]] inline bool a32_is_exclusive(uint32_t w) {
  return (w >> 28U) != 0xfU && (w & 0x0f800ff0U) == 0x01800090U;
}

[[nodiscard]] inline bool a32_is_unsupported_sys(uint32_t w) {
  const uint32_t cond = w >> 28U;
  if (cond == 0xfU) {
    if ((w & 0xfe000000U) == 0xfa000000U) // BLX imm
      return false;
    // Barriers (DMB/DSB/ISB) and PLD/PLI/CLREX without PC-rel.
    if ((w & 0xfffffff0U) == 0xf57ff040U || // DSB
        (w & 0xfffffff0U) == 0xf57ff050U || // DMB
        (w & 0xfffffff0U) == 0xf57ff060U || // ISB
        w == 0xf57ff01fU)                   // CLREX
      return false;
    return true;
  }
  // Reject BKPT, HVC, SMC, ERET, UDF, MRS/MSR? (Allow SVC, VFP/NEON non-PC).
  if ((w & 0x0ff000f0U) == 0x01200070U || // BKPT
      (w & 0x0ff000f0U) == 0x01400070U || // HVC
      (w & 0x0ff000f0U) == 0x01600070U || // SMC
      (w & 0x0ff000f0U) == 0x07f000f0U || // UDF
      (w & 0x0fb00000U) == 0x03200000U || // MSR imm / hints? allow NOP/YIELD
      (w & 0x0f900000U) == 0x01000000U) { // misc sys
    if ((w & 0x0fffffffU) == 0x0320f000U) // NOP
      return false;
    if ((w & 0x0ffffff0U) == 0x012fff10U || // BX Rm
        (w & 0x0ffffff0U) == 0x012fff30U || // BLX Rm
        (w & 0x0fff0ff0U) == 0x016f0f10U)   // CLZ
      return false;
    return true;
  }
  return false;
}

[[nodiscard]] inline Error decode_a32(uint32_t w, uint32_t addr,
                                      Instruction *out) {
  *out = Instruction{};
  out->width = 4U;
  out->raw = w;
  const uint32_t cond = w >> 28U;
  const uint32_t pc = addr + 8U;

  if (cond == 0xfU) {
    if ((w & 0xfe000000U) == 0xfa000000U) {
      out->kind = Kind::ArmBlxImm;
      const int32_t delta = sign_extend32(
          ((w & 0x00ffffffU) << 2U) | (((w >> 24U) & 1U) << 1U), 26U);
      out->target =
          static_cast<uint32_t>(pc + static_cast<uint32_t>(delta)) | 1U;
      return Error::None;
    }
    if (!a32_is_unsupported_sys(w)) {
      out->kind = Kind::Copy;
      return Error::None;
    }
    return Error::Unsupported;
  }

  if (a32_is_exclusive(w))
    return Error::Unsupported;

  // B / BL (A1)
  if ((w & 0x0e000000U) == 0x0a000000U) {
    const bool link = (w & 0x01000000U) != 0U;
    out->kind = link ? Kind::ArmCall : Kind::ArmBranch;
    out->is_terminal = !link && cond == 0xeU;
    const int32_t delta = sign_extend32((w & 0x00ffffffU) << 2U, 26U);
    out->target = pc + static_cast<uint32_t>(delta);
    return Error::None;
  }

  // BX / BLX Rm
  if ((w & 0x0ffffff0U) == 0x012fff10U || (w & 0x0ffffff0U) == 0x012fff30U) {
    const unsigned rm = w & 0xfU;
    const bool link = (w & 0x20U) != 0U;
    if (rm == 15U) {
      if (link)
        return Error::Unsupported;
      out->kind = Kind::ArmBxPc;
      out->target = pc;
      out->is_terminal = cond == 0xeU;
      return Error::None;
    }
    out->kind = Kind::Copy;
    out->is_terminal = !link && cond == 0xeU;
    return Error::None;
  }

  // ADR A1 (ADD Rd, PC, #imm) / A2 (SUB Rd, PC, #imm)
  if ((w & 0x0fff0000U) == 0x028f0000U || (w & 0x0fff0000U) == 0x024f0000U) {
    const unsigned rd = (w >> 12U) & 0xfU;
    if ((w & 0x00100000U) != 0U || rd == 15U)
      return Error::Unsupported;
    const uint32_t imm = arm_expand_imm(w & 0xfffU);
    const bool add = (w & 0x00800000U) != 0U;
    out->kind = Kind::ArmAdr;
    out->target = add ? (pc + imm) : (pc - imm);
    return Error::None;
  }

  // Literal loads: LDR / LDRB (imm12), !W (bit21 == 0 or P == 1)
  if ((w & 0x0f3f0000U) == 0x051f0000U) {
    const bool byte = (w & 0x00400000U) != 0U;
    const bool add = (w & 0x00800000U) != 0U;
    const bool writeback = (w & 0x00200000U) != 0U;
    const unsigned rt = (w >> 12U) & 0xfU;
    if (writeback || (byte && rt == 15U))
      return Error::Unsupported;
    const uint32_t imm = w & 0xfffU;
    out->kind = Kind::ArmLiteral;
    out->target = add ? (pc + imm) : (pc - imm);
    out->access_width = byte ? 1U : 4U;
    out->is_terminal = !byte && rt == 15U && cond == 0xeU;
    return Error::None;
  }

  // Extra literal loads: LDRD / LDRH / LDRSB / LDRSH (P=1, I=1, W=0, Rn=15)
  if ((w & 0x0f7f0090U) == 0x014f0090U) {
    const unsigned op2 = (w >> 5U) & 3U;
    const bool l_bit = (w & 0x00100000U) != 0U;
    const unsigned rt = (w >> 12U) & 0xfU;
    // LDRD: L=0, op2=2; LDRH: L=1, op2=1; LDRSB: L=1, op2=2; LDRSH: L=1, op2=3.
    uint8_t width = 0;
    if (!l_bit && op2 == 2U && (rt & 1U) == 0U && rt < 14U)
      width = 8U;
    else if (l_bit && op2 == 1U && rt != 15U)
      width = 2U;
    else if (l_bit && op2 == 2U && rt != 15U)
      width = 1U;
    else if (l_bit && op2 == 3U && rt != 15U)
      width = 2U;
    else
      return Error::Unsupported;
    const bool add = (w & 0x00800000U) != 0U;
    const uint32_t imm = (((w >> 8U) & 0xfU) << 4U) | (w & 0xfU);
    out->kind = Kind::ArmLiteral;
    out->target = add ? (pc + imm) : (pc - imm);
    out->access_width = width;
    return Error::None;
  }

  // VLDR Sd/Dd, [PC, ±imm]
  if ((w & 0x0f3f0e00U) == 0x0d1f0a00U) {
    const bool dbl = (w & 0x00000100U) != 0U;
    const bool add = (w & 0x00800000U) != 0U;
    const uint32_t imm = (w & 0xffU) << 2U;
    out->kind = Kind::ArmVldrLit;
    out->target = add ? (pc + imm) : (pc - imm);
    out->access_width = dbl ? 8U : 4U;
    return Error::None;
  }

  // ADD / SUB / MOV register with PC
  if ((w & 0x0fe00010U) == 0x00800000U || // ADD reg (shift by imm)
      (w & 0x0fe00010U) == 0x00400000U || // SUB reg (shift by imm)
      (w & 0x0fef0010U) == 0x01a00000U) { // MOV reg (shift by imm)
    const unsigned rn = (w >> 16U) & 0xfU;
    const unsigned rd = (w >> 12U) & 0xfU;
    const unsigned rm = w & 0xfU;
    const bool s_bit = (w & 0x00100000U) != 0U;
    const bool is_mov = (w & 0x0fe00000U) == 0x01a00000U;
    if ((!is_mov && (rn == 15U || rm == 15U)) || (is_mov && rm == 15U)) {
      if (s_bit && rd == 15U)
        return Error::Unsupported;
      if (!is_mov && rn == 13U)
        return Error::Unsupported;
      out->kind = Kind::ArmAluPc;
      out->target = pc;
      out->is_terminal = rd == 15U && cond == 0xeU;
      return Error::None;
    }
  }

  // LDR/B/H/SB/SH/D register-indexed with Rn == PC
  if (((w & 0x0f3f0010U) == 0x071f0000U) || // LDR/LDRB reg
      ((w & 0x0f7f0090U) == 0x010f0090U)) { // LDRD/LDRH/LDRSB/LDRSH reg
    const bool writeback = (w & 0x00200000U) != 0U;
    if (writeback)
      return Error::Unsupported;
    const unsigned rt = (w >> 12U) & 0xfU;
    const bool is_extra = (w & 0x0e000000U) == 0U;
    if (is_extra) {
      const unsigned op2 = (w >> 5U) & 3U;
      const bool l_bit = (w & 0x00100000U) != 0U;
      const bool valid_ldrd =
          !l_bit && op2 == 2U && (rt & 1U) == 0U && rt < 14U;
      const bool valid_half = l_bit && op2 >= 1U && rt != 15U;
      if (!valid_ldrd && !valid_half)
        return Error::Unsupported;
    } else if ((w & 0x00400000U) != 0U && rt == 15U) {
      return Error::Unsupported;
    }
    out->kind = Kind::ArmLdrRegPc;
    out->target = pc;
    out->is_terminal =
        !is_extra && (w & 0x00400000U) == 0U && rt == 15U && cond == 0xeU;
    return Error::None;
  }

  // Reject any remaining PC-reading or PC-writing instructions.
  if (a32_is_unsupported_sys(w))
    return Error::Unsupported;
  const unsigned group = w & 0x0e000000U;
  if (group == 0x08000000U) {             // LDM / STM
    if ((w & 0x000f0000U) == 0x000f0000U) // Rn == PC
      return Error::Unsupported;
    if ((w & 0x00108000U) == 0x00108000U) { // LDM with PC
      out->is_terminal = cond == 0xeU;
    } else if ((w & 0x00008000U) != 0U) { // STM with PC
      return Error::Unsupported;
    }
    out->kind = Kind::Copy;
    return Error::None;
  }
  if (group == 0x0c000000U || group == 0x0e000000U) {
    // Allow VFP/NEON register/stack/non-PC memory operations.
    if ((w & 0x0f000e00U) == 0x0e000a00U || // VFP DP / VMOV
        ((w & 0x0e000e00U) == 0x0c000a00U &&
         ((w >> 16U) & 0xfU) != 15U)) { // VLDM/VSTM/VPUSH/VPOP/VLDR/VSTR non-PC
      out->kind = Kind::Copy;
      return Error::None;
    }
    return Error::Unsupported;
  }
  if (group <= 0x06000000U) {
    const unsigned rn = (w >> 16U) & 0xfU;
    const unsigned rd = (w >> 12U) & 0xfU;
    if (rn == 15U || rd == 15U)
      return Error::Unsupported;
    if (group <= 0x02000000U) {
      if ((w & 0x02000000U) == 0U && (w & 0xfU) == 15U)
        return Error::Unsupported;
      if ((w & 0x02000010U) == 0x10U && ((w >> 8U) & 0xfU) == 15U)
        return Error::Unsupported;
    } else if ((w & 0x02000000U) != 0U && (w & 0xfU) == 15U) {
      return Error::Unsupported;
    }
    out->kind = Kind::Copy;
    return Error::None;
  }
  return Error::Unsupported;
}

[[nodiscard]] inline Error decode_t16(uint16_t hw, uint32_t addr,
                                      Instruction *out) {
  *out = Instruction{};
  out->width = 2U;
  out->raw = hw;
  const uint32_t pc = addr + 4U;

  // Reject BKPT, UDF, HLT, CPS, SETEND, IT (IT is handled in group decoder).
  if ((hw & 0xff00U) == 0xbe00U || // BKPT
      (hw & 0xff00U) == 0xde00U || // UDF
      (hw & 0xffc0U) == 0xba80U || // HLT? (check: 0xb000..0xbfff misc)
      (hw & 0xffe0U) == 0xb660U || // CPS
      (hw & 0xfff7U) == 0xb650U || // SETEND
      thumb_is_it(hw)) {
    return Error::Unsupported;
  }

  // B<c> T1
  if ((hw & 0xf000U) == 0xd000U) {
    const unsigned cond = (hw >> 8U) & 0xfU;
    if (cond == 0xeU || cond == 0xfU) // UDF / SVC
      return cond == 0xfU ? (out->kind = Kind::Copy, Error::None)
                          : Error::Unsupported;
    out->kind = Kind::ThumbBranch16Cond;
    out->target =
        (pc + static_cast<uint32_t>(sign_extend32((hw & 0xffU) << 1U, 9U))) |
        1U;
    return Error::None;
  }

  // B T2
  if ((hw & 0xf800U) == 0xe000U) {
    out->kind = Kind::ThumbBranch16;
    out->target =
        (pc + static_cast<uint32_t>(sign_extend32((hw & 0x7ffU) << 1U, 12U))) |
        1U;
    out->is_terminal = true;
    return Error::None;
  }

  // CBZ / CBNZ
  if ((hw & 0xf500U) == 0xb100U) {
    const uint32_t imm =
        (((hw >> 9U) & 1U) << 6U) | (((hw >> 3U) & 0x1fU) << 1U);
    out->kind = Kind::ThumbCbz;
    out->target = (pc + imm) | 1U;
    return Error::None;
  }

  // ADR T1
  if ((hw & 0xf800U) == 0xa000U) {
    out->kind = Kind::ThumbAdr16;
    out->target = align4_pc(addr, 4U) + ((hw & 0xffU) << 2U);
    return Error::None;
  }

  // LDR Rt, [PC, #imm] T1
  if ((hw & 0xf800U) == 0x4800U) {
    out->kind = Kind::ThumbLdrLit16;
    out->target = align4_pc(addr, 4U) + ((hw & 0xffU) << 2U);
    out->access_width = 4U;
    return Error::None;
  }

  // Special data / BX / BLX (0x4400..0x47ff)
  if ((hw & 0xfc00U) == 0x4400U) {
    const unsigned op = (hw >> 8U) & 3U;
    const unsigned rm = (hw >> 3U) & 0xfU;
    const unsigned rd = (hw & 7U) | ((hw >> 4U) & 8U);
    if (op == 3U) { // BX / BLX Rm
      const bool link = (hw & 0x80U) != 0U;
      if (rm == 15U) {
        if (link)
          return Error::Unsupported;
        out->kind = Kind::ThumbBxPc;
        out->target = align4_pc(addr, 4U);
        out->is_terminal = true;
        return Error::None;
      }
      out->kind = Kind::Copy;
      out->is_terminal = !link;
      return Error::None;
    }
    if (op == 0U && (rm == 15U || rd == 15U)) { // ADD Rd, Rm
      if (rm == 15U && rd != 13U && rd != 15U) {
        out->kind = Kind::ThumbAddPc16;
        out->target = pc;
        return Error::None;
      }
      return Error::Unsupported;
    }
    if (op == 1U && (rm == 15U || rd == 15U)) // CMP with PC
      return Error::Unsupported;
    if (op == 2U && (rm == 15U || rd == 15U)) { // MOV Rd, Rm
      if (rm == 15U && rd != 13U) {
        out->kind = Kind::ThumbMovPc16;
        out->target = pc | (rd == 15U ? 1U : 0U);
        out->is_terminal = rd == 15U;
        return Error::None;
      }
      return Error::Unsupported;
    }
    out->kind = Kind::Copy;
    return Error::None;
  }

  // POP {..., PC}
  if ((hw & 0xff00U) == 0xbd00U) {
    out->kind = Kind::Copy;
    out->is_terminal = true;
    return Error::None;
  }

  out->kind = Kind::Copy;
  return Error::None;
}

[[nodiscard]] inline Error decode_t32(uint16_t hw1, uint16_t hw2, uint32_t addr,
                                      Instruction *out) {
  *out = Instruction{};
  out->width = 4U;
  out->raw = hw1;
  out->raw_hi = hw2;
  const uint32_t insn = (uint32_t{hw1} << 16U) | hw2;
  const uint32_t pc = addr + 4U;

  // Branches and miscellaneous control (hw1[15:11] == 0b11110)
  if ((hw1 & 0xf800U) == 0xf000U) {
    if ((hw2 & 0x8000U) != 0U) {
      const uint32_t s = (hw1 >> 10U) & 1U;
      const uint32_t j1 = (hw2 >> 13U) & 1U;
      const uint32_t j2 = (hw2 >> 11U) & 1U;
      if ((hw2 & 0x5000U) == 0x0000U) {
        // B.W<c> T3 (unless hw1[9:7] == 111 -> misc / UDF / barriers / MRS /
        // MSR)
        if ((hw1 & 0x0380U) == 0x0380U) {
          // Barriers DSB/DMB/ISB/CLREX, NOP.W, MRS
          if (hw1 == 0xf3bfU && ((hw2 & 0xfff0U) == 0x8f20U ||  // CLREX
                                 (hw2 & 0xfff0U) == 0x8f40U ||  // DSB
                                 (hw2 & 0xfff0U) == 0x8f50U ||  // DMB
                                 (hw2 & 0xfff0U) == 0x8f60U)) { // ISB
            out->kind = Kind::Copy;
            return Error::None;
          }
          if (hw1 == 0xf3afU && (hw2 & 0xff00U) == 0x8000U &&
              (hw2 & 0xffU) <= 4U) { // NOP.W / YIELD / WFE / WFI / SEV
            out->kind = Kind::Copy;
            return Error::None;
          }
          if (hw1 == 0xf3efU && (hw2 & 0xf000U) == 0x8000U) { // MRS
            out->kind = Kind::Copy;
            return Error::None;
          }
          return Error::Unsupported;
        }
        const uint32_t raw = (s << 20U) | (j2 << 19U) | (j1 << 18U) |
                             ((hw1 & 0x3fU) << 12U) | ((hw2 & 0x7ffU) << 1U);
        out->kind = Kind::ThumbBranch32Cond;
        out->target =
            (pc + static_cast<uint32_t>(sign_extend32(raw, 21U))) | 1U;
        return Error::None;
      }
      // B.W T4 / BL / BLX
      const uint32_t i1 = (~(j1 ^ s)) & 1U;
      const uint32_t i2 = (~(j2 ^ s)) & 1U;
      const uint32_t raw = (s << 24U) | (i1 << 23U) | (i2 << 22U) |
                           ((hw1 & 0x3ffU) << 12U) | ((hw2 & 0x7ffU) << 1U);
      const int32_t delta = sign_extend32(raw, 25U);
      if ((hw2 & 0xd000U) == 0x9000U) {
        out->kind = Kind::ThumbBranch32;
        out->target = (pc + static_cast<uint32_t>(delta)) | 1U;
        out->is_terminal = true;
        return Error::None;
      }
      if ((hw2 & 0xd000U) == 0xd000U) {
        out->kind = Kind::ThumbBl32;
        out->target = (pc + static_cast<uint32_t>(delta)) | 1U;
        return Error::None;
      }
      if ((hw2 & 0xd000U) == 0xc000U) {
        out->kind = Kind::ThumbBlx32;
        out->target =
            align4_pc(addr, 4U) +
            static_cast<uint32_t>(sign_extend32(raw & ~uint32_t{2}, 25U));
        return Error::None;
      }
      return Error::Unsupported;
    }

    // ADR.W T2 (SUBW Rd, PC, #imm12) / T3 (ADDW Rd, PC, #imm12)
    if ((insn & 0xfbff8000U) == 0xf2af0000U ||
        (insn & 0xfbff8000U) == 0xf20f0000U) {
      const unsigned rd = (hw2 >> 8U) & 0xfU;
      if (rd == 13U || rd == 15U)
        return Error::Unsupported;
      const bool sub = (hw1 & 0x00a0U) == 0x00a0U;
      const uint32_t imm = (((hw1 >> 10U) & 1U) << 11U) |
                           (((hw2 >> 12U) & 7U) << 8U) | (hw2 & 0xffU);
      out->kind = Kind::ThumbAdr32;
      out->target =
          sub ? (align4_pc(addr, 4U) - imm) : (align4_pc(addr, 4U) + imm);
      return Error::None;
    }

    // Data processing modified/plain immediate (MOVW/MOVT/ADD.W/SUB.W/...)
    const unsigned rn = hw1 & 0xfU;
    const unsigned rd = (hw2 >> 8U) & 0xfU;
    const bool plain_imm = (hw1 & 0x0200U) != 0U;
    if (!plain_imm) {
      // Modified imm: Rn == 15 is valid only for MOV/MVN (op == 2/3)
      const unsigned op = (hw1 >> 5U) & 0xfU;
      if (rn == 15U && op != 2U && op != 3U)
        return Error::Unsupported;
      if (rd == 15U && ((hw1 & 0x10U) == 0U ||
                        (op != 0U && op != 4U && op != 8U && op != 13U)))
        return Error::Unsupported;
    } else {
      // Plain imm: MOVW/MOVT encode immediate bits in Rn.
      const bool is_movwt = (hw1 & 0xfb70U) == 0xf240U;
      if (!is_movwt && rn == 15U)
        return Error::Unsupported;
      if (rd == 15U)
        return Error::Unsupported;
    }
    out->kind = Kind::Copy;
    return Error::None;
  }

  // TBB / TBH
  if ((insn & 0xfff0fff0U) == 0xe8d0f000U ||
      (insn & 0xfff0fff0U) == 0xe8d0f010U) {
    out->kind = Kind::ThumbTb32;
    out->target = pc;
    out->is_terminal = true;
    return Error::None;
  }

  // Reject LDREX* / STREX*
  if ((hw1 & 0xffe0U) == 0xe840U || (hw1 & 0xffe0U) == 0xe8c0U)
    return Error::Unsupported;

  // LDRD literal (Rn == 15)
  if ((hw1 & 0xff7fU) == 0xe95fU) {
    const unsigned rt = (hw2 >> 12U) & 0xfU;
    const unsigned rt2 = (hw2 >> 8U) & 0xfU;
    if (rt >= 14U || rt2 >= 14U)
      return Error::Unsupported;
    const bool add = (hw1 & 0x0080U) != 0U;
    const uint32_t imm = (hw2 & 0xffU) << 2U;
    out->kind = Kind::ThumbLiteral32;
    out->target =
        add ? (align4_pc(addr, 4U) + imm) : (align4_pc(addr, 4U) - imm);
    out->access_width = 8U;
    return Error::None;
  }

  // LDM / STM / PUSH.W / POP.W / LDRD / STRD (non-PC)
  if ((hw1 & 0xfe40U) == 0xe800U) {
    if ((hw1 & 0x001fU) == 0x001fU) // LDM/STM with Rn == PC
      return Error::Unsupported;
    if ((hw1 & 0x0010U) == 0U && (hw2 & 0x8000U) != 0U) // STM with PC
      return Error::Unsupported;
    if ((hw1 & 0x0010U) != 0U && (hw2 & 0x8000U) != 0U) // POP.W / LDM with PC
      out->is_terminal = true;
    out->kind = Kind::Copy;
    return Error::None;
  }

  // Data processing shifted register (0xea00..0xebff)
  if ((hw1 & 0xfe00U) == 0xea00U) {
    const unsigned rn = hw1 & 0xfU;
    const unsigned rd = (hw2 >> 8U) & 0xfU;
    const unsigned rm = hw2 & 0xfU;
    const unsigned op = (hw1 >> 5U) & 0xfU;
    const bool s_bit = (hw1 & 0x0010U) != 0U;
    // Allow CMP/TST/TEQ/CMN where Rd == 15 && S == 1; allow MOV/MVN where Rn
    // == 15.
    const bool rd_pc_test =
        rd == 15U && s_bit && (op == 0U || op == 4U || op == 8U || op == 13U);
    const bool rn_pc_mov = rn == 15U && (op == 2U || op == 3U);
    if ((rn == 15U && !rn_pc_mov) || (rd == 15U && !rd_pc_test) || rm == 15U)
      return Error::Unsupported;
    out->kind = Kind::Copy;
    return Error::None;
  }

  // VFP / NEON (0xec00..0xefff, 0xfc00..0xffff)
  if ((hw1 & 0xee00U) == 0xec00U) {
    if ((hw1 & 0xff3fU) == 0xed1fU && (hw2 & 0x0c00U) == 0x0800U) {
      // VLDR Sd/Dd, [PC, ±imm]
      const bool dbl = (hw2 & 0x0100U) != 0U;
      const bool add = (hw1 & 0x0080U) != 0U;
      const uint32_t imm = (hw2 & 0xffU) << 2U;
      out->kind = Kind::ThumbVldrLit32;
      out->target =
          add ? (align4_pc(addr, 4U) + imm) : (align4_pc(addr, 4U) - imm);
      out->access_width = dbl ? 8U : 4U;
      return Error::None;
    }
    if ((hw1 & 0x0f00U) == 0x0c00U || (hw1 & 0x0f00U) == 0x0d00U) {
      if ((hw1 & 0xfU) == 15U)
        return Error::Unsupported;
    }
    out->kind = Kind::Copy;
    return Error::None;
  }

  // Single load/store & PLD/PLI literal (0xf800..0xf9ff)
  if ((hw1 & 0xfe00U) == 0xf800U) {
    const unsigned rn = hw1 & 0xfU;
    const unsigned rt = (hw2 >> 12U) & 0xfU;
    if (rn == 15U) {
      const bool add = (hw1 & 0x0080U) != 0U;
      const uint32_t imm = hw2 & 0xfffU;
      const uint32_t target =
          add ? (align4_pc(addr, 4U) + imm) : (align4_pc(addr, 4U) - imm);
      const unsigned size_bits = (hw1 >> 5U) & 3U;
      const bool is_load = (hw1 & 0x0010U) != 0U;
      const bool is_signed = (hw1 & 0x0100U) != 0U;
      if (!is_load)
        return Error::Unsupported;
      if (rt == 15U && !is_signed && size_bits == 2U) {
        // LDR.W PC, [PC, ±imm]
        out->kind = Kind::ThumbLiteral32;
        out->target = target;
        out->access_width = 4U;
        out->is_terminal = true;
        return Error::None;
      }
      if (rt == 15U && size_bits == 0U) {
        // PLD / PLI literal
        out->kind = Kind::ThumbPldPliLit32;
        out->target = target;
        return Error::None;
      }
      if (rt == 15U || rt == 13U)
        return Error::Unsupported;
      // LDRB(0), LDRH(1), LDR(2), LDRSB(signed 0), LDRSH(signed 1)
      if (size_bits == 3U || (is_signed && size_bits == 2U))
        return Error::Unsupported;
      out->kind = Kind::ThumbLiteral32;
      out->target = target;
      out->access_width = size_bits == 0U ? 1U : (size_bits == 1U ? 2U : 4U);
      return Error::None;
    }
    out->kind = Kind::Copy;
    return Error::None;
  }

  // Data processing register / multiply / divide (0xfa00..0xfbff)
  if ((hw1 & 0xfe00U) == 0xfa00U) {
    const unsigned rn = hw1 & 0xfU;
    const unsigned rd = (hw2 >> 8U) & 0xfU;
    const unsigned rm = hw2 & 0xfU;
    if (rn == 15U || rd == 15U || rm == 15U)
      return Error::Unsupported;
    out->kind = Kind::Copy;
    return Error::None;
  }

  return Error::Unsupported;
}

[[nodiscard]] inline uint8_t a32_pick_scratch(uint32_t w,
                                              uint8_t exclude_extra = 16U) {
  const uint8_t rn = (w >> 16U) & 0xfU;
  const uint8_t rd = (w >> 12U) & 0xfU;
  const uint8_t rm = w & 0xfU;
  for (uint8_t r = 3U; r < 4U; --r) {
    if (r != rn && r != rd && r != rm && r != exclude_extra)
      return r;
  }
  return 0xffU;
}

[[nodiscard]] inline uint8_t a32_pick_ry(uint32_t w, uint8_t rx) {
  const uint8_t rn = (w >> 16U) & 0xfU;
  const uint8_t rm = w & 0xfU;
  for (uint8_t r = 4U; r < 5U; --r) {
    if (r != rx && r != rn && r != rm)
      return r;
  }
  return 0xffU;
}

[[nodiscard]] inline bool tb32_pick_scratches(uint16_t hw1, uint16_t hw2,
                                              uint8_t *rx, uint8_t *ry) {
  const uint8_t rn = hw1 & 0xfU;
  const uint8_t rm = hw2 & 0xfU;
  *rx = 0xffU;
  *ry = 0xffU;
  for (uint8_t r = 7U; r < 8U; --r) {
    if (r == rn || r == rm)
      continue;
    if (*rx == 0xffU) {
      *rx = r;
    } else {
      *ry = r;
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline uint32_t a32_make_uncond_ldr(uint32_t orig, uint8_t rt,
                                                  uint8_t rn) {
  // Strip cond -> AL (0xe), set P=1, U=1, W=0, B/L/S/H preserved, imm=0.
  if ((orig & 0x0e000000U) == 0x04000000U) {
    return 0xe5900000U | (orig & 0x00400000U) | (uint32_t{rn} << 16U) |
           (uint32_t{rt} << 12U);
  }
  // Extra load: LDRD / LDRH / LDRSB / LDRSH with imm=0, P=1, U=1, I=1, W=0.
  return 0xe1c00090U | (orig & 0x00100060U) | (uint32_t{rn} << 16U) |
         (uint32_t{rt} << 12U);
}

[[nodiscard]] inline uint32_t t32_make_uncond_ldr(uint16_t hw1, uint16_t hw2,
                                                  uint8_t rt, uint8_t rn) {
  if ((hw1 & 0xff7fU) == 0xe95fU) {
    // LDRD Rt, Rt2, [Rn, #0]
    const uint8_t rt2 = (hw2 >> 8U) & 0xfU;
    return (uint32_t{0xe9d0U | rn} << 16U) |
           ((uint32_t{rt} << 12U) | (uint32_t{rt2} << 8U));
  }
  // LDR{,B,H,SB,SH}.W Rt, [Rn, #0] (U=1)
  return (uint32_t{(hw1 & 0xfff0U) | 0x0080U | rn} << 16U) |
         (uint32_t{rt} << 12U);
}

[[nodiscard]] inline bool thumb_sets_flags_in_it(uint16_t hw1, uint16_t hw2,
                                                 bool is32) {
  if (!is32) {
    return (hw1 & 0xf800U) == 0x2800U || // CMP imm8
           (hw1 & 0xffc0U) == 0x4200U || // TST reg
           (hw1 & 0xffc0U) == 0x4280U || // CMP reg
           (hw1 & 0xffc0U) == 0x42c0U || // CMN reg
           (hw1 & 0xff00U) == 0x4500U;   // CMP high reg
  }
  if ((hw1 & 0xffe0U) == 0xf380U) // MSR
    return true;
  if (hw1 == 0xeef1U && (hw2 & 0xf000U) == 0xf000U) // VMRS APSR_nzcv
    return true;
  if ((hw1 & 0xf600U) == 0xf000U && (hw2 & 0x8000U) == 0U &&
      (hw1 & 0x0010U) != 0U)
    return true;
  if ((hw1 & 0xfe00U) == 0xea00U && (hw1 & 0x0010U) != 0U)
    return true;
  return false;
}

[[nodiscard]] inline bool thumb_it_pc_rel_supported(const Instruction &sub,
                                                    bool is_last) {
  switch (sub.kind) {
  case Kind::ThumbBranch16Cond:
  case Kind::ThumbBranch16:
  case Kind::ThumbBranch32Cond:
  case Kind::ThumbBranch32:
    return is_last;
  case Kind::ThumbAdr16:
  case Kind::ThumbAdr32:
    return true;
  case Kind::ThumbMovPc16:
    return is_last || ((sub.raw & 7U) != 7U || ((sub.raw >> 4U) & 8U) == 0U);
  case Kind::ThumbLdrLit16:
    return true;
  case Kind::ThumbLiteral32: {
    const bool to_pc =
        (sub.raw & 0xff7fU) != 0xe95fU && ((sub.raw_hi >> 12U) & 0xfU) == 15U;
    return is_last || !to_pc;
  }
  default:
    return false;
  }
}

} // namespace detail

[[nodiscard]] inline Error analyze(const uint8_t *bytes, size_t available,
                                   uintptr_t address, size_t minimum,
                                   bool thumb, Plan *out) {
  if (out == nullptr || bytes == nullptr || minimum == 0 || minimum > 12 ||
      (!thumb && (address & 3U) != 0) || (thumb && (address & 1U) != 0) ||
      address > UINT32_MAX - kMaxCoveredBytes)
    return Error::InvalidInput;
  *out = Plan{};
  out->covered_size = static_cast<uint8_t>(minimum);
  out->thumb = thumb;

  const uint32_t base_addr = static_cast<uint32_t>(address);
  size_t copied = 0;
  while (copied < minimum) {
    if (out->count == kMaxInsns || copied + 2U > available)
      return out->error = Error::Capacity;
    Instruction insn{};
    insn.offset = static_cast<uint8_t>(copied);
    const uint32_t cur_addr = base_addr + static_cast<uint32_t>(copied);
    if (!thumb) {
      if (copied + 4U > available)
        return out->error = Error::Capacity;
      uint32_t word = 0;
      memcpy(&word, bytes + copied, sizeof(word));
      const Error err = detail::decode_a32(word, cur_addr, &insn);
      if (err != Error::None)
        return out->error = err;
      if (insn.kind == Kind::ArmLdrRegPc || insn.kind == Kind::ArmAluPc) {
        const uint8_t rx = detail::a32_pick_scratch(
            word, (word & 0x0e000000U) == 0U && (word & 0x00100060U) == 0x40U
                      ? (((word >> 12U) & 0xfU) + 1U)
                      : 16U);
        if (rx == 0xffU)
          return out->error = Error::Unsupported;
        if (((word >> 12U) & 0xfU) == 15U &&
            detail::a32_pick_ry(word, rx) == 0xffU)
          return out->error = Error::Unsupported;
      }
    } else {
      uint16_t first = 0;
      memcpy(&first, bytes + copied, sizeof(first));
      if (thumb_is_it(first)) {
        const unsigned firstcond = (first >> 4U) & 0xfU;
        if (firstcond == 0xfU)
          return out->error = Error::Unsupported;
        const unsigned it_count = thumb_it_count(first);
        if (firstcond == 0xeU && it_count > 1U)
          return out->error = Error::Unsupported;
        size_t span = 2U;
        bool all_copy = true;
        Instruction sub[4]{};
        bool sub_sets_flags[4]{};
        size_t sub_off[4]{};
        for (unsigned k = 0; k < it_count; ++k) {
          if (copied + span + 2U > available)
            return out->error = Error::Capacity;
          uint16_t hw = 0;
          memcpy(&hw, bytes + copied + span, sizeof(hw));
          const uint32_t inner_addr =
              base_addr + static_cast<uint32_t>(copied + span);
          sub_off[k] = span;
          if (thumb_is_32bit(hw)) {
            if (copied + span + 4U > available)
              return out->error = Error::Capacity;
            uint16_t hw2 = 0;
            memcpy(&hw2, bytes + copied + span + 2U, sizeof(hw2));
            const Error err = detail::decode_t32(hw, hw2, inner_addr, &sub[k]);
            if (err != Error::None)
              return out->error = err;
            sub_sets_flags[k] = detail::thumb_sets_flags_in_it(hw, hw2, true);
            span += 4U;
          } else {
            const Error err = detail::decode_t16(hw, inner_addr, &sub[k]);
            if (err != Error::None)
              return out->error = err;
            sub_sets_flags[k] = detail::thumb_sets_flags_in_it(hw, 0, false);
            span += 2U;
          }
          if (sub[k].kind != Kind::Copy)
            all_copy = false;
        }
        if (span > sizeof(insn.it_bytes) || copied + span > kMaxCoveredBytes)
          return out->error = Error::Capacity;
        if (all_copy) {
          insn.kind = Kind::ThumbItGroup;
          insn.width = static_cast<uint8_t>(span);
          insn.it_cond = static_cast<uint8_t>(firstcond);
          insn.it_count = static_cast<uint8_t>(it_count);
          insn.it_bytes_len = static_cast<uint8_t>(span);
          memcpy(insn.it_bytes, bytes + copied, span);
        } else if (it_count == 1U &&
                   detail::thumb_it_pc_rel_supported(sub[0], true)) {
          insn = sub[0];
          insn.offset = static_cast<uint8_t>(copied);
          insn.width = static_cast<uint8_t>(span);
          insn.it_single_kind = sub[0].kind;
          insn.kind = Kind::ThumbItSingle;
          insn.it_cond = static_cast<uint8_t>(firstcond);
          insn.it_count = 1U;
          insn.is_terminal = false;
        } else if (it_count >= 2U && out->count + it_count <= kMaxInsns) {
          for (unsigned k = 0; k < it_count; ++k) {
            const bool is_last = k + 1U == it_count;
            if (!is_last && (sub_sets_flags[k] || sub[k].is_terminal))
              return out->error = Error::Unsupported;
            if (sub[k].kind != Kind::Copy &&
                !detail::thumb_it_pc_rel_supported(sub[k], is_last))
              return out->error = Error::Unsupported;
            const unsigned cond_k =
                k == 0U ? firstcond
                        : ((firstcond & 0xeU) | ((first >> (4U - k)) & 1U));
            if (cond_k >= 14U)
              return out->error = Error::Unsupported;
            Instruction item{};
            if (sub[k].kind == Kind::Copy) {
              const uint16_t it1 =
                  static_cast<uint16_t>(0xbf00U | (cond_k << 4U) | 0x8U);
              item.kind = Kind::ThumbItGroup;
              item.offset = static_cast<uint8_t>(copied + sub_off[k]);
              item.width = sub[k].width;
              item.it_cond = static_cast<uint8_t>(cond_k);
              item.it_count = 1U;
              item.it_bytes_len = static_cast<uint8_t>(2U + sub[k].width);
              detail::write_u16(item.it_bytes, 0, it1);
              memcpy(item.it_bytes + 2U, bytes + copied + sub_off[k],
                     sub[k].width);
            } else {
              item = sub[k];
              item.offset = static_cast<uint8_t>(copied + sub_off[k]);
              item.it_single_kind = sub[k].kind;
              item.kind = Kind::ThumbItSingle;
              item.it_cond = static_cast<uint8_t>(cond_k);
              item.it_count = 1U;
              item.is_terminal = false;
            }
            if (k == 0U)
              item.offset = static_cast<uint8_t>(copied);
            out->instructions[out->count++] = item;
          }
          copied += span;
          continue;
        } else {
          return out->error = Error::Unsupported;
        }
      } else if (thumb_is_32bit(first)) {
        if (copied + 4U > available)
          return out->error = Error::Capacity;
        uint16_t second = 0;
        memcpy(&second, bytes + copied + 2U, sizeof(second));
        const Error err = detail::decode_t32(first, second, cur_addr, &insn);
        if (err != Error::None)
          return out->error = err;
        if (insn.kind == Kind::ThumbTb32) {
          uint8_t rx = 0, ry = 0;
          if (!detail::tb32_pick_scratches(first, second, &rx, &ry))
            return out->error = Error::Unsupported;
        }
      } else {
        const Error err = detail::decode_t16(first, cur_addr, &insn);
        if (err != Error::None)
          return out->error = err;
      }
    }
    if (copied + insn.width > kMaxCoveredBytes)
      return out->error = Error::Capacity;
    out->instructions[out->count++] = insn;
    copied += insn.width;
  }

  out->copied_size = static_cast<uint8_t>(copied);

  for (size_t i = 0; i != out->count; ++i) {
    const auto &insn = out->instructions[i];
    const Kind k =
        insn.kind == Kind::ThumbItSingle ? insn.it_single_kind : insn.kind;
    if (insn.access_width != 0 &&
        overlaps(insn.target, insn.access_width, base_addr, copied))
      return out->error = Error::Overlap;
    if ((k == Kind::ArmAdr || k == Kind::ThumbAdr16 || k == Kind::ThumbAdr32) &&
        overlaps(insn.target, 1U, base_addr, copied))
      return out->error = Error::Overlap;
    if (k == Kind::ArmCall || k == Kind::ArmBlxImm || k == Kind::ThumbBl32 ||
        k == Kind::ThumbBlx32) {
      const uint32_t raw_target = insn.target & ~uint32_t{1};
      if (raw_target >= base_addr && raw_target < base_addr + copied)
        return out->error = Error::Unsupported;
    }
    if (k == Kind::ArmBranch || k == Kind::ThumbBranch16Cond ||
        k == Kind::ThumbBranch16 || k == Kind::ThumbCbz ||
        k == Kind::ThumbBranch32Cond || k == Kind::ThumbBranch32) {
      const uint32_t raw_target = insn.target & ~uint32_t{1};
      if (raw_target >= base_addr && raw_target < base_addr + copied) {
        if ((thumb && (insn.target & 1U) == 0U) ||
            (!thumb && (insn.target & 3U) != 0U))
          return out->error = Error::Unsupported;
        bool lands_on_insn = false;
        for (size_t j = 0; j != out->count; ++j) {
          if (raw_target == base_addr + out->instructions[j].offset) {
            lands_on_insn = true;
            break;
          }
        }
        if (!lands_on_insn)
          return out->error = Error::Unsupported;
      }
    }
  }

  out->needs_continuation = !out->instructions[out->count - 1U].is_terminal;
  return Error::None;
}

[[nodiscard]] inline uint32_t mapped_target(const Plan &plan, uint32_t source,
                                            uint32_t destination,
                                            uint32_t target) {
  const uint32_t untagged = target & ~uint32_t{1};
  for (size_t i = 0; i != plan.count; ++i) {
    if (untagged == source + plan.instructions[i].offset)
      return (destination + plan.offsets[i]) | (plan.thumb ? 1U : 0U);
  }
  return target;
}

namespace detail {

[[nodiscard]] inline uint8_t compute_length(const Plan &plan, size_t index,
                                            uint32_t source_addr,
                                            uint32_t dest_addr) {
  const auto &insn = plan.instructions[index];
  const uint32_t at = dest_addr + plan.offsets[index];
  const uint32_t pad = plan.thumb && (at & 2U) != 0U ? 2U : 0U;
  const Kind k =
      insn.kind == Kind::ThumbItSingle ? insn.it_single_kind : insn.kind;
  const uint32_t target =
      mapped_target(plan, source_addr, dest_addr, insn.target);

  if (!plan.thumb) {
    switch (k) {
    case Kind::Copy:
      return 4U;
    case Kind::ArmBranch: {
      if (a32_b_reachable(at, target))
        return 4U;
      const uint8_t cond = static_cast<uint8_t>(insn.raw >> 28U);
      return cond == 0xeU ? 8U : 12U;
    }
    case Kind::ArmCall:
    case Kind::ArmBlxImm: {
      const bool blx = k == Kind::ArmBlxImm;
      if (!blx && a32_b_reachable(at, target))
        return 4U;
      if (blx) {
        uint32_t dummy = 0;
        if (encode_a32_blx(at, target & ~uint32_t{1}, &dummy))
          return 4U;
      }
      const uint8_t cond = blx ? 0xeU : static_cast<uint8_t>(insn.raw >> 28U);
      return cond == 0xeU ? 12U : 16U;
    }
    case Kind::ArmBxPc: {
      const uint8_t cond = static_cast<uint8_t>(insn.raw >> 28U);
      return cond == 0xeU ? 8U : 12U;
    }
    case Kind::ArmAdr:
      return 12U;
    case Kind::ArmLiteral: {
      const uint8_t cond = static_cast<uint8_t>(insn.raw >> 28U);
      const unsigned rt = (insn.raw >> 12U) & 0xfU;
      if (rt == 15U)
        return cond == 0xeU ? 28U : 36U;
      return cond == 0xeU ? 16U : 24U;
    }
    case Kind::ArmVldrLit: {
      const uint8_t cond = static_cast<uint8_t>(insn.raw >> 28U);
      return cond == 0xeU ? 24U : 32U;
    }
    case Kind::ArmAluPc:
    case Kind::ArmLdrRegPc: {
      const uint8_t cond = static_cast<uint8_t>(insn.raw >> 28U);
      return cond == 0xeU ? 24U : 32U;
    }
    default:
      return 4U;
    }
  }

  // Thumb mode
  uint8_t body = 0;
  switch (k) {
  case Kind::Copy:
    return insn.width;
  case Kind::ThumbItGroup:
    return insn.it_bytes_len;
  case Kind::ThumbBranch16Cond:
    if (insn.kind != Kind::ThumbItSingle &&
        t16_b_cond_reachable(at, target & ~uint32_t{1}))
      return 2U;
    if (insn.kind != Kind::ThumbItSingle &&
        t32_b_cond_reachable(at, target & ~uint32_t{1}))
      return 4U;
    body = 12U;
    break;
  case Kind::ThumbBranch16:
    if (insn.kind != Kind::ThumbItSingle &&
        t16_b_reachable(at, target & ~uint32_t{1}))
      return 2U;
    if (insn.kind != Kind::ThumbItSingle &&
        t32_b_reachable(at, target & ~uint32_t{1}))
      return 4U;
    body =
        (insn.kind == Kind::ThumbItSingle && insn.it_cond != 0xeU) ? 12U : 8U;
    break;
  case Kind::ThumbCbz:
    if (t16_cbz_reachable(at, target & ~uint32_t{1}))
      return 2U;
    body = 12U;
    break;
  case Kind::ThumbBxPc:
    body = 8U;
    break;
  case Kind::ThumbBranch32Cond:
    if (insn.kind != Kind::ThumbItSingle &&
        t32_b_cond_reachable(at, target & ~uint32_t{1}))
      return 4U;
    body = 12U;
    break;
  case Kind::ThumbBranch32:
    if (insn.kind != Kind::ThumbItSingle &&
        t32_b_reachable(at, target & ~uint32_t{1}))
      return 4U;
    body =
        (insn.kind == Kind::ThumbItSingle && insn.it_cond != 0xeU) ? 12U : 8U;
    break;
  case Kind::ThumbBl32:
    if (t32_b_reachable(at, target & ~uint32_t{1}))
      return 4U;
    body = 12U;
    break;
  case Kind::ThumbBlx32:
    if (t32_blx_reachable(at, target & ~uint32_t{1}))
      return 4U;
    body = 12U;
    break;
  case Kind::ThumbAdr16:
  case Kind::ThumbAdr32:
  case Kind::ThumbMovPc16:
    if (k == Kind::ThumbMovPc16 && (insn.raw & 7U) == 7U &&
        ((insn.raw >> 4U) & 8U) != 0U) {
      body = 8U;
    } else if (insn.kind == Kind::ThumbItSingle && insn.it_cond != 0xeU) {
      body = 16U;
    } else {
      body = 12U;
    }
    break;
  case Kind::ThumbLdrLit16:
  case Kind::ThumbLiteral32: {
    const bool to_pc = k == Kind::ThumbLiteral32 &&
                       (insn.raw & 0xff7fU) != 0xe95fU &&
                       ((insn.raw_hi >> 12U) & 0xfU) == 15U;
    if (to_pc)
      body = 24U;
    else if (insn.kind == Kind::ThumbItSingle && insn.it_cond != 0xeU)
      body = 20U;
    else
      body = 16U;
    break;
  }
  case Kind::ThumbAddPc16:
    body = 20U;
    break;
  case Kind::ThumbPldPliLit32:
  case Kind::ThumbVldrLit32:
    body = 22U;
    break;
  case Kind::ThumbTb32:
    body = 32U;
    break;
  default:
    return 4U;
  }
  return static_cast<uint8_t>(pad + body);
}

} // namespace detail

[[nodiscard]] inline Error plan(const uint8_t *bytes, size_t available,
                                uintptr_t source_address, uintptr_t destination,
                                size_t capacity, size_t minimum, bool thumb,
                                Plan *out) {
  const Error analyzed =
      analyze(bytes, available, source_address, minimum, thumb, out);
  if (analyzed != Error::None)
    return analyzed;
  const uint32_t src = static_cast<uint32_t>(source_address);
  const uint32_t dst = static_cast<uint32_t>(destination);
  if (capacity < 8 || destination > UINT32_MAX - capacity ||
      (!thumb && (dst & 3U) != 0U) || (thumb && (dst & 1U) != 0U))
    return out->error = Error::Capacity;

  for (size_t i = 0; i != out->count; ++i)
    out->lengths[i] = out->instructions[i].width;

  for (unsigned pass = 0; pass != 12U; ++pass) {
    bool changed = false;
    uint16_t cursor = 0;
    for (size_t i = 0; i != out->count; ++i) {
      out->offsets[i] = cursor;
      cursor = static_cast<uint16_t>(cursor + out->lengths[i]);
    }
    for (size_t i = 0; i != out->count; ++i) {
      const uint8_t len = detail::compute_length(*out, i, src, dst);
      if (len != out->lengths[i]) {
        out->lengths[i] = len;
        changed = true;
      }
    }
    if (!changed) {
      size_t tail = 0;
      if (out->needs_continuation) {
        const uint32_t tail_at = dst + cursor;
        const uint32_t resume = src + out->copied_size;
        if (!thumb) {
          tail = a32_b_reachable(tail_at, resume) ? 4U : 8U;
        } else {
          tail = t32_b_reachable(tail_at, resume)
                     ? 4U
                     : ((tail_at & 2U) != 0U ? 10U : 8U);
        }
      }
      const size_t total = static_cast<size_t>(cursor) + tail;
      if (total > capacity || total > UINT16_MAX)
        return out->error = Error::Capacity;
      out->code_size = static_cast<uint16_t>(total);
      return Error::None;
    }
  }
  return out->error = Error::Unreachable;
}

[[nodiscard]] inline Error emit(const Plan &plan, uintptr_t source_address,
                                uintptr_t destination, uint8_t *out,
                                size_t capacity) {
  if (plan.error != Error::None || out == nullptr || plan.count == 0 ||
      plan.code_size > capacity)
    return Error::Capacity;
  const uint32_t src = static_cast<uint32_t>(source_address);
  const uint32_t dst = static_cast<uint32_t>(destination);

  for (size_t i = 0; i != plan.count; ++i) {
    const auto &insn = plan.instructions[i];
    const size_t off = plan.offsets[i];
    const uint32_t at = dst + static_cast<uint32_t>(off);
    const uint8_t len = plan.lengths[i];
    const Kind k =
        insn.kind == Kind::ThumbItSingle ? insn.it_single_kind : insn.kind;
    const uint32_t target = mapped_target(plan, src, dst, insn.target);

    if (!plan.thumb) {
      const uint32_t w = insn.raw;
      const uint8_t cond = static_cast<uint8_t>(w >> 28U);
      const uint32_t cfield = uint32_t{cond} << 28U;
      switch (k) {
      case Kind::Copy:
        detail::write_u32(out, off, w);
        break;
      case Kind::ArmBranch: {
        uint32_t enc = 0;
        if (len == 4U &&
            detail::encode_a32_branch(at, target, cond, false, &enc)) {
          detail::write_u32(out, off, enc);
        } else if (cond == 0xeU) {
          detail::write_u32(out, off, 0xe51ff004U);
          detail::write_u32(out, off + 4U, target);
        } else {
          detail::write_u32(out, off, 0x059ff000U | cfield);
          detail::write_u32(out, off + 4U, 0xea000000U);
          detail::write_u32(out, off + 8U, target);
        }
        break;
      }
      case Kind::ArmCall:
      case Kind::ArmBlxImm: {
        const bool blx = k == Kind::ArmBlxImm;
        uint32_t enc = 0;
        if (len == 4U) {
          const bool ok =
              blx ? detail::encode_a32_blx(at, target & ~uint32_t{1}, &enc)
                  : detail::encode_a32_branch(at, target, cond, true, &enc);
          if (!ok)
            return Error::Unreachable;
          detail::write_u32(out, off, enc);
        } else if (blx || cond == 0xeU) {
          detail::write_u32(out, off, 0xe28fe004U);      // add lr, pc, #4
          detail::write_u32(out, off + 4U, 0xe51ff004U); // ldr pc, [pc, #-4]
          detail::write_u32(out, off + 8U, target);
        } else {
          detail::write_u32(out, off, 0x028fe008U | cfield);
          detail::write_u32(out, off + 4U, 0x059ff000U | cfield);
          detail::write_u32(out, off + 8U, 0xea000000U);
          detail::write_u32(out, off + 12U, target);
        }
        break;
      }
      case Kind::ArmBxPc:
        if (cond == 0xeU) {
          detail::write_u32(out, off, 0xe51ff004U);
          detail::write_u32(out, off + 4U, target);
        } else {
          detail::write_u32(out, off, 0x059ff000U | cfield);
          detail::write_u32(out, off + 4U, 0xea000000U);
          detail::write_u32(out, off + 8U, target);
        }
        break;
      case Kind::ArmAdr: {
        const uint32_t rd = (w >> 12U) & 0xfU;
        detail::write_u32(out, off, 0x059f0000U | cfield | (rd << 12U));
        detail::write_u32(out, off + 4U, 0xea000000U);
        detail::write_u32(out, off + 8U, target);
        break;
      }
      case Kind::ArmLiteral: {
        const uint8_t rt = static_cast<uint8_t>((w >> 12U) & 0xfU);
        size_t cur = off;
        if (cond != 0xeU) {
          const uint32_t skip_words = rt == 15U ? 6U : 3U;
          detail::write_u32(out, cur, 0x0a000000U | cfield);
          detail::write_u32(out, cur + 4U, 0xea000000U | skip_words);
          cur += 8U;
        }
        if (rt == 15U) {
          detail::write_u32(out, cur, 0xe92d0003U);      // push {r0, r1}
          detail::write_u32(out, cur + 4U, 0xe59f0000U); // ldr r0, [pc, #0]
          detail::write_u32(out, cur + 8U, 0xea000000U); // b #0
          detail::write_u32(out, cur + 12U, target);
          detail::write_u32(out, cur + 16U, 0xe5900000U); // ldr r0, [r0]
          detail::write_u32(out, cur + 20U, 0xe58d0004U); // str r0, [sp, #4]
          detail::write_u32(out, cur + 24U, 0xe8bd8001U); // pop {r0, pc}
        } else {
          detail::write_u32(out, cur, 0xe59f0000U | (uint32_t{rt} << 12U));
          detail::write_u32(out, cur + 4U, 0xea000000U);
          detail::write_u32(out, cur + 8U, target);
          detail::write_u32(out, cur + 12U,
                            detail::a32_make_uncond_ldr(w, rt, rt));
        }
        break;
      }
      case Kind::ArmVldrLit: {
        size_t cur = off;
        if (cond != 0xeU) {
          detail::write_u32(out, cur, 0x0a000000U | cfield);
          detail::write_u32(out, cur + 4U, 0xea000005U);
          cur += 8U;
        }
        detail::write_u32(out, cur, 0xe52d0004U);      // push {r0}
        detail::write_u32(out, cur + 4U, 0xe59f0000U); // ldr r0, [pc, #0]
        detail::write_u32(out, cur + 8U, 0xea000000U); // b #0
        detail::write_u32(out, cur + 12U, target);
        detail::write_u32(out, cur + 16U,
                          0xed900a00U | (w & 0x0040f100U)); // vldr s/d, [r0]
        detail::write_u32(out, cur + 20U, 0xe49d0004U);     // pop {r0}
        break;
      }
      case Kind::ArmAluPc:
      case Kind::ArmLdrRegPc: {
        const uint8_t rd = static_cast<uint8_t>((w >> 12U) & 0xfU);
        const uint8_t extra = k == Kind::ArmLdrRegPc &&
                                      (w & 0x0e000000U) == 0U &&
                                      (w & 0x00100060U) == 0x40U
                                  ? static_cast<uint8_t>(rd + 1U)
                                  : 16U;
        const uint8_t rx = detail::a32_pick_scratch(w, extra);
        if (rx == 0xffU)
          return Error::Unsupported;
        size_t cur = off;
        if (cond != 0xeU) {
          detail::write_u32(out, cur, 0x0a000000U | cfield);
          detail::write_u32(out, cur + 4U, 0xea000005U);
          cur += 8U;
        }
        uint32_t rewritten = (w & 0x0fffffffU) | 0xe0000000U;
        if (k == Kind::ArmAluPc) {
          if (((w >> 16U) & 0xfU) == 15U && (w & 0x0fe00000U) != 0x01a00000U)
            rewritten = (rewritten & 0xfff0ffffU) | (uint32_t{rx} << 16U);
          if ((w & 0xfU) == 15U)
            rewritten = (rewritten & 0xfffffff0U) | rx;
        } else {
          rewritten = (rewritten & 0xfff0ffffU) | (uint32_t{rx} << 16U);
        }
        if (rd == 15U) {
          const uint8_t ry = detail::a32_pick_ry(w, rx);
          if (ry == 0xffU)
            return Error::Unsupported;
          rewritten = (rewritten & 0xffff0fffU) | (uint32_t{ry} << 12U);
          const uint32_t mask = (uint32_t{1} << rx) | (uint32_t{1} << ry);
          detail::write_u32(out, cur, 0xe92d8000U | mask); // push {rx, ry, pc}
          detail::write_u32(out, cur + 4U,
                            0xe59f0008U |
                                (uint32_t{rx} << 12U)); // ldr rx, [pc, #8]
          detail::write_u32(out, cur + 8U, rewritten);
          detail::write_u32(out, cur + 12U,
                            0xe58d0008U |
                                (uint32_t{ry} << 12U)); // str ry, [sp, #8]
          detail::write_u32(out, cur + 16U,
                            0xe8bd8000U | mask); // pop {rx, ry, pc}
          detail::write_u32(out, cur + 20U, target);
        } else {
          detail::write_u32(out, cur,
                            0xe52d0004U | (uint32_t{rx} << 12U)); // push {rx}
          detail::write_u32(out, cur + 4U,
                            0xe59f0008U |
                                (uint32_t{rx} << 12U)); // ldr rx, [pc, #8]
          detail::write_u32(out, cur + 8U, rewritten);
          detail::write_u32(out, cur + 12U,
                            0xe49d0004U | (uint32_t{rx} << 12U)); // pop {rx}
          detail::write_u32(out, cur + 16U, 0xea000000U);         // b #0
          detail::write_u32(out, cur + 20U, target);
        }
        break;
      }
      default:
        return Error::Unsupported;
      }
      continue;
    }

    // Thumb mode
    size_t cur = off;
    const bool direct_short = (k == Kind::Copy) || (k == Kind::ThumbItGroup) ||
                              (k == Kind::ThumbBranch16Cond && len == 2U) ||
                              (k == Kind::ThumbBranch16 && len <= 4U) ||
                              (k == Kind::ThumbCbz && len == 2U) ||
                              (k == Kind::ThumbBranch32Cond && len == 4U) ||
                              (k == Kind::ThumbBranch32 && len == 4U) ||
                              (k == Kind::ThumbBl32 && len == 4U) ||
                              (k == Kind::ThumbBlx32 && len == 4U);
    if (!direct_short && (at & 2U) != 0U) {
      detail::write_u16(out, cur, 0xbf00U);
      cur += 2U;
    }

    const uint8_t single_cond =
        insn.kind == Kind::ThumbItSingle ? insn.it_cond : 0xeU;
    const bool need_guard =
        insn.kind == Kind::ThumbItSingle && single_cond != 0xeU;

    switch (k) {
    case Kind::Copy:
      if (insn.width == 2U)
        detail::write_u16(out, cur, static_cast<uint16_t>(insn.raw));
      else
        detail::write_t32(out, cur, static_cast<uint16_t>(insn.raw),
                          static_cast<uint16_t>(insn.raw_hi));
      break;
    case Kind::ThumbItGroup:
      memcpy(out + cur, insn.it_bytes, insn.it_bytes_len);
      break;
    case Kind::ThumbBranch16Cond:
    case Kind::ThumbBranch32Cond: {
      const uint8_t cond = k == Kind::ThumbBranch16Cond
                               ? static_cast<uint8_t>((insn.raw >> 8U) & 0xfU)
                               : static_cast<uint8_t>((insn.raw >> 6U) & 0xfU);
      if (len == 2U) {
        const uint32_t imm = (target & ~uint32_t{1}) - (at + 4U);
        detail::write_u16(out, cur,
                          static_cast<uint16_t>(0xd000U |
                                                (uint32_t{cond} << 8U) |
                                                ((imm >> 1U) & 0xffU)));
      } else if (len == 4U) {
        uint16_t hw1 = 0, hw2 = 0;
        if (!detail::encode_t32_b_t3(at, target & ~uint32_t{1}, cond, &hw1,
                                     &hw2))
          return Error::Unreachable;
        detail::write_t32(out, cur, hw1, hw2);
      } else {
        const uint8_t inv = cond ^ 1U;
        detail::write_u16(
            out, cur, static_cast<uint16_t>(0xd004U | (uint32_t{inv} << 8U)));
        detail::write_u16(out, cur + 2U, 0xbf00U);
        detail::write_t32(out, cur + 4U, 0xf8dfU, 0xf000U);
        detail::write_u32(out, cur + 8U, target);
      }
      break;
    }
    case Kind::ThumbBranch16:
    case Kind::ThumbBranch32: {
      if (len == 2U) {
        const uint32_t imm = (target & ~uint32_t{1}) - (at + 4U);
        detail::write_u16(
            out, cur, static_cast<uint16_t>(0xe000U | ((imm >> 1U) & 0x7ffU)));
      } else if (len == 4U) {
        uint16_t hw1 = 0, hw2 = 0;
        if (!detail::encode_t32_b_t4(at, target & ~uint32_t{1}, &hw1, &hw2))
          return Error::Unreachable;
        detail::write_t32(out, cur, hw1, hw2);
      } else if (need_guard) {
        detail::write_u16(out, cur,
                          static_cast<uint16_t>(
                              0xd004U | (uint32_t{single_cond ^ 1U} << 8U)));
        detail::write_u16(out, cur + 2U, 0xbf00U);
        detail::write_t32(out, cur + 4U, 0xf8dfU, 0xf000U);
        detail::write_u32(out, cur + 8U, target);
      } else {
        detail::write_t32(out, cur, 0xf8dfU, 0xf000U);
        detail::write_u32(out, cur + 4U, target);
      }
      break;
    }
    case Kind::ThumbCbz: {
      if (len == 2U) {
        const uint32_t imm = (target & ~uint32_t{1}) - (at + 4U);
        const uint16_t i_bit = static_cast<uint16_t>(((imm >> 6U) & 1U) << 9U);
        const uint16_t imm5 =
            static_cast<uint16_t>(((imm >> 1U) & 0x1fU) << 3U);
        detail::write_u16(
            out, cur,
            static_cast<uint16_t>((insn.raw & 0xfd07U) | i_bit | imm5));
      } else {
        detail::write_u16(out, cur,
                          static_cast<uint16_t>(
                              ((insn.raw & 0xfd07U) ^ 0x0800U) | (4U << 3U)));
        detail::write_u16(out, cur + 2U, 0xbf00U);
        detail::write_t32(out, cur + 4U, 0xf8dfU, 0xf000U);
        detail::write_u32(out, cur + 8U, target);
      }
      break;
    }
    case Kind::ThumbBxPc:
      detail::write_t32(out, cur, 0xf8dfU, 0xf000U);
      detail::write_u32(out, cur + 4U, target);
      break;
    case Kind::ThumbBl32:
    case Kind::ThumbBlx32: {
      if (len == 4U) {
        uint16_t hw1 = 0, hw2 = 0;
        const bool ok = k == Kind::ThumbBl32
                            ? detail::encode_t32_bl_t1(
                                  at, target & ~uint32_t{1}, &hw1, &hw2)
                            : detail::encode_t32_blx_t2(
                                  at, target & ~uint32_t{1}, &hw1, &hw2);
        if (!ok)
          return Error::Unreachable;
        detail::write_t32(out, cur, hw1, hw2);
      } else {
        detail::write_t32(out, cur, 0xf20fU, 0x0e09U); // addw lr, pc, #9
        detail::write_t32(out, cur + 4U, 0xf8dfU, 0xf000U);
        detail::write_u32(out, cur + 8U, target);
      }
      break;
    }
    case Kind::ThumbAdr16:
    case Kind::ThumbAdr32:
    case Kind::ThumbMovPc16: {
      const uint8_t rd =
          k == Kind::ThumbAdr16
              ? static_cast<uint8_t>((insn.raw >> 8U) & 7U)
              : (k == Kind::ThumbAdr32
                     ? static_cast<uint8_t>((insn.raw_hi >> 8U) & 0xfU)
                     : static_cast<uint8_t>((insn.raw & 7U) |
                                            ((insn.raw >> 4U) & 8U)));
      if (rd == 15U) {
        detail::write_t32(out, cur, 0xf8dfU, 0xf000U);
        detail::write_u32(out, cur + 4U, target);
        break;
      }
      if (need_guard) {
        detail::write_u16(out, cur,
                          static_cast<uint16_t>(
                              0xd006U | (uint32_t{single_cond ^ 1U} << 8U)));
        detail::write_u16(out, cur + 2U, 0xbf00U);
        cur += 4U;
      }
      detail::write_t32(out, cur, 0xf8dfU,
                        static_cast<uint16_t>((uint32_t{rd} << 12U) | 4U));
      detail::write_u16(out, cur + 4U, 0xe002U);
      detail::write_u16(out, cur + 6U, 0xbf00U);
      detail::write_u32(out, cur + 8U, target);
      break;
    }
    case Kind::ThumbLdrLit16:
    case Kind::ThumbLiteral32: {
      const bool is_ldrd =
          k == Kind::ThumbLiteral32 && (insn.raw & 0xff7fU) == 0xe95fU;
      const uint8_t rt =
          k == Kind::ThumbLdrLit16
              ? static_cast<uint8_t>((insn.raw >> 8U) & 7U)
              : static_cast<uint8_t>((insn.raw_hi >> 12U) & 0xfU);
      if (rt == 15U) {
        detail::write_u16(out, cur, 0xb403U);      // push {r0, r1}
        detail::write_u16(out, cur + 2U, 0xbf00U); // nop
        detail::write_t32(out, cur + 4U, 0xf8dfU, 0x0004U);
        detail::write_u16(out, cur + 8U, 0xe002U);
        detail::write_u16(out, cur + 10U, 0xbf00U);
        detail::write_u32(out, cur + 12U, target);
        detail::write_t32(out, cur + 16U, 0xf8d0U, 0x0000U); // ldr.w r0, [r0]
        detail::write_u16(out, cur + 20U, 0x9001U);          // str r0, [sp, #4]
        detail::write_u16(out, cur + 22U, 0xbd01U);          // pop {r0, pc}
        break;
      }
      if (need_guard) {
        detail::write_u16(out, cur,
                          static_cast<uint16_t>(
                              0xd008U | (uint32_t{single_cond ^ 1U} << 8U)));
        detail::write_u16(out, cur + 2U, 0xbf00U);
        cur += 4U;
      }
      detail::write_t32(out, cur, 0xf8dfU,
                        static_cast<uint16_t>((uint32_t{rt} << 12U) | 4U));
      detail::write_u16(out, cur + 4U, 0xe002U);
      detail::write_u16(out, cur + 6U, 0xbf00U);
      detail::write_u32(out, cur + 8U, target);
      if (k == Kind::ThumbLdrLit16) {
        detail::write_u16(
            out, cur + 12U,
            static_cast<uint16_t>(0x6800U | (uint32_t{rt} << 3U) | rt));
        detail::write_u16(out, cur + 14U, 0xbf00U);
      } else if (is_ldrd) {
        const uint32_t ldrd = detail::t32_make_uncond_ldr(
            static_cast<uint16_t>(insn.raw), static_cast<uint16_t>(insn.raw_hi),
            rt, rt);
        detail::write_t32(out, cur + 12U, static_cast<uint16_t>(ldrd >> 16U),
                          static_cast<uint16_t>(ldrd & 0xffffU));
      } else {
        const uint32_t ldr = detail::t32_make_uncond_ldr(
            static_cast<uint16_t>(insn.raw), static_cast<uint16_t>(insn.raw_hi),
            rt, rt);
        detail::write_t32(out, cur + 12U, static_cast<uint16_t>(ldr >> 16U),
                          static_cast<uint16_t>(ldr & 0xffffU));
      }
      break;
    }
    case Kind::ThumbAddPc16: {
      const uint8_t rd =
          static_cast<uint8_t>((insn.raw & 7U) | ((insn.raw >> 4U) & 8U));
      const uint8_t rx = rd == 0U ? 1U : 0U;
      detail::write_u16(out, cur, static_cast<uint16_t>(0xb400U | (1U << rx)));
      detail::write_u16(out, cur + 2U, 0xbf00U);
      detail::write_t32(out, cur + 4U, 0xf8dfU,
                        static_cast<uint16_t>((uint32_t{rx} << 12U) | 8U));
      detail::write_u16(out, cur + 8U,
                        static_cast<uint16_t>(0x4400U | ((rd & 8U) << 4U) |
                                              (uint32_t{rx} << 3U) |
                                              (rd & 7U)));
      detail::write_u16(out, cur + 10U,
                        static_cast<uint16_t>(0xbc00U | (1U << rx)));
      detail::write_u16(out, cur + 12U, 0xe002U);
      detail::write_u16(out, cur + 14U, 0xbf00U);
      detail::write_u32(out, cur + 16U, target);
      break;
    }
    case Kind::ThumbPldPliLit32:
    case Kind::ThumbVldrLit32: {
      detail::write_u16(out, cur, 0xb401U);      // push {r0}
      detail::write_u16(out, cur + 2U, 0xbf00U); // nop
      detail::write_t32(out, cur + 4U, 0xf8dfU, 0x0004U);
      detail::write_u16(out, cur + 8U, 0xe002U);
      detail::write_u16(out, cur + 10U, 0xbf00U);
      detail::write_u32(out, cur + 12U, target);
      if (k == Kind::ThumbPldPliLit32) {
        detail::write_t32(out, cur + 16U,
                          static_cast<uint16_t>((insn.raw & 0xfff0U) | 0x0080U),
                          0xf000U);
      } else {
        detail::write_t32(out, cur + 16U,
                          static_cast<uint16_t>((insn.raw & 0xff70U) | 0x0080U),
                          static_cast<uint16_t>(insn.raw_hi & 0xff00U));
      }
      detail::write_u16(out, cur + 20U, 0xbc01U); // pop {r0}
      break;
    }
    case Kind::ThumbTb32: {
      const uint16_t hw1 = static_cast<uint16_t>(insn.raw);
      const uint16_t hw2 = static_cast<uint16_t>(insn.raw_hi);
      uint8_t rx = 0, ry = 0;
      if (!detail::tb32_pick_scratches(hw1, hw2, &rx, &ry))
        return Error::Unsupported;
      const uint8_t rn = hw1 & 0xfU;
      const uint8_t rm = hw2 & 0xfU;
      const uint8_t h_bit = (hw2 >> 4U) & 1U;
      const uint8_t base = rn == 15U ? rx : rn;
      const uint16_t mask = static_cast<uint16_t>((1U << rx) | (1U << ry));
      detail::write_u16(out, cur, static_cast<uint16_t>(0xb500U | mask));
      detail::write_u16(out, cur + 2U, 0xbf00U);
      detail::write_t32(out, cur + 4U, 0xf8dfU,
                        static_cast<uint16_t>((uint32_t{rx} << 12U) | 20U));
      detail::write_t32(out, cur + 8U, static_cast<uint16_t>(0xeb00U | base),
                        static_cast<uint16_t>((uint32_t{ry} << 8U) |
                                              (uint32_t{h_bit} << 6U) | rm));
      detail::write_u16(
          out, cur + 12U,
          static_cast<uint16_t>((h_bit != 0U ? 0x8800U : 0x7800U) |
                                (uint32_t{ry} << 3U) | ry));
      detail::write_u16(out, cur + 14U, 0xbf00U);
      detail::write_t32(
          out, cur + 16U, static_cast<uint16_t>(0xeb00U | rx),
          static_cast<uint16_t>((uint32_t{rx} << 8U) | 0x0040U | ry));
      detail::write_u16(out, cur + 20U,
                        static_cast<uint16_t>(0x3001U | (uint32_t{rx} << 8U)));
      detail::write_u16(out, cur + 22U,
                        static_cast<uint16_t>(0x9002U | (uint32_t{rx} << 8U)));
      detail::write_u16(out, cur + 24U, static_cast<uint16_t>(0xbd00U | mask));
      detail::write_u16(out, cur + 26U, 0xbf00U);
      detail::write_u32(out, cur + 28U, target);
      break;
    }
    default:
      return Error::Unsupported;
    }
  }

  if (plan.needs_continuation) {
    const size_t tail_off =
        plan.offsets[plan.count - 1U] + plan.lengths[plan.count - 1U];
    const uint32_t tail_at = dst + static_cast<uint32_t>(tail_off);
    const uint32_t resume = src + plan.copied_size;
    if (!plan.thumb) {
      uint32_t enc = 0;
      if (detail::encode_a32_branch(tail_at, resume, 0xeU, false, &enc)) {
        detail::write_u32(out, tail_off, enc);
      } else {
        detail::write_u32(out, tail_off, 0xe51ff004U);
        detail::write_u32(out, tail_off + 4U, resume);
      }
    } else {
      uint16_t hw1 = 0, hw2 = 0;
      if (detail::encode_t32_b_t4(tail_at, resume, &hw1, &hw2)) {
        detail::write_t32(out, tail_off, hw1, hw2);
      } else {
        size_t cur = tail_off;
        if ((tail_at & 2U) != 0U) {
          detail::write_u16(out, cur, 0xbf00U);
          cur += 2U;
        }
        detail::write_t32(out, cur, 0xf8dfU, 0xf000U);
        detail::write_u32(out, cur + 4U, resume | 1U);
      }
    }
  }
  return Error::None;
}

[[nodiscard]] inline size_t emit_entry_island(uint8_t *out, size_t capacity,
                                              uintptr_t island_addr,
                                              uintptr_t target_callable,
                                              bool thumb) {
  if (out == nullptr)
    return 0;
  if (!thumb) {
    if (capacity < 8U || (island_addr & 3U) != 0)
      return 0;
    detail::write_u32(out, 0U, 0xe51ff004U); // ldr pc, [pc, #-4]
    detail::write_u32(out, 4U, static_cast<uint32_t>(target_callable));
    return 8U;
  }
  const size_t pad = (island_addr & 2U) != 0 ? 2U : 0U;
  if (capacity < pad + 8U)
    return 0;
  if (pad != 0)
    detail::write_u16(out, 0U, 0xbf00U);         // nop
  detail::write_t32(out, pad, 0xf8dfU, 0xf000U); // ldr.w pc, [pc, #0]
  detail::write_u32(out, pad + 4U, static_cast<uint32_t>(target_callable));
  return pad + 8U;
}

[[nodiscard]] inline bool resolve_thunk(const uint8_t *code, size_t available,
                                        uintptr_t address, bool thumb,
                                        uintptr_t *resolved) {
  if (code == nullptr || resolved == nullptr)
    return false;
  if (!thumb) {
    if ((address & 3U) != 0 || available < 4U)
      return false;
    uint32_t w = 0;
    memcpy(&w, code, sizeof(w));
    Instruction insn{};
    if (detail::decode_a32(w, static_cast<uint32_t>(address), &insn) !=
        Error::None)
      return false;
    if (insn.kind == Kind::ArmBranch && (w >> 28U) == 0xeU) {
      *resolved = insn.target;
      return true;
    }
    if (w == 0xe51ff004U && available >= 8U) {
      uint32_t dst = 0;
      memcpy(&dst, code + 4U, sizeof(dst));
      if (dst != 0) {
        *resolved = dst;
        return true;
      }
    }
    return false;
  }

  if (available < 4U)
    return false;
  uint16_t hw1 = 0;
  uint16_t hw2 = 0;
  memcpy(&hw1, code, sizeof(hw1));
  if (thumb_is_32bit(hw1)) {
    memcpy(&hw2, code + 2U, sizeof(hw2));
    Instruction insn{};
    if (detail::decode_t32(hw1, hw2, static_cast<uint32_t>(address), &insn) ==
            Error::None &&
        insn.kind == Kind::ThumbBranch32) {
      *resolved = insn.target;
      return true;
    }
  }
  size_t off = 0;
  if (hw1 == 0xbf00U) {
    off = 2U;
    if (available < 10U)
      return false;
    memcpy(&hw1, code + off, sizeof(hw1));
  }
  if (available >= off + 8U && hw1 == 0xf8dfU) {
    memcpy(&hw2, code + off + 2U, sizeof(hw2));
    if (hw2 == 0xf000U && ((address + off) & 2U) == 0) {
      uint32_t dst = 0;
      memcpy(&dst, code + off + 4U, sizeof(dst));
      if (dst != 0) {
        *resolved = dst;
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] inline bool has_interior_branch(const uint8_t *code,
                                              size_t scan_bytes,
                                              uintptr_t base_addr,
                                              size_t covered_size, bool thumb) {
  if (code == nullptr || scan_bytes <= covered_size ||
      (!thumb && (covered_size <= 4U || (base_addr & 3U) != 0)) ||
      (thumb && (covered_size <= 2U || (base_addr & 1U) != 0)) ||
      base_addr > UINT32_MAX - scan_bytes)
    return false;
  const uint32_t base = static_cast<uint32_t>(base_addr);
  const size_t limit = scan_bytes < 4096U ? scan_bytes : 4096U;
  uint32_t furthest = base + static_cast<uint32_t>(covered_size);
  struct LiteralSpan {
    uint32_t begin = 0;
    uint32_t end = 0;
  };
  LiteralSpan spans[32]{};
  size_t span_count = 0;
  const auto record_lit = [&](const Instruction &insn, uint32_t pc) {
    if (insn.access_width == 0 || span_count >= 32U)
      return;
    const uint32_t untagged = insn.target;
    if (untagged > pc &&
        untagged <= base + static_cast<uint32_t>(limit) - insn.access_width) {
      spans[span_count++] = {untagged, untagged + insn.access_width};
    }
  };
  const auto in_literal = [&](uint32_t pc) -> bool {
    for (size_t s = 0; s != span_count; ++s) {
      if (pc >= spans[s].begin && pc < spans[s].end)
        return true;
    }
    return false;
  };

  if (!thumb) {
    const size_t aligned = limit & ~size_t{3};
    for (size_t offset = 0; offset + 4U <= aligned; offset += 4U) {
      const uint32_t pc = base + static_cast<uint32_t>(offset);
      if (in_literal(pc))
        continue;
      uint32_t w = 0;
      memcpy(&w, code + offset, sizeof(w));
      Instruction insn{};
      if (detail::decode_a32(w, pc, &insn) == Error::None) {
        record_lit(insn, pc);
        const bool is_direct = insn.kind == Kind::ArmBranch ||
                               insn.kind == Kind::ArmCall ||
                               insn.kind == Kind::ArmBlxImm;
        if (is_direct) {
          const uint32_t raw_target = insn.target & ~uint32_t{1};
          if (offset >= covered_size && raw_target > base &&
              raw_target < base + static_cast<uint32_t>(covered_size))
            return true;
          if (insn.kind == Kind::ArmBranch && raw_target > furthest &&
              raw_target < base + static_cast<uint32_t>(aligned))
            furthest = raw_target;
        }
        if (offset >= covered_size && insn.is_terminal && pc + 4U >= furthest)
          return false;
      } else if (offset >= covered_size &&
                 ((w & 0x0ff000f0U) == 0x01200070U ||
                  (w & 0x0ff000f0U) == 0x07f000f0U) &&
                 pc + 4U >= furthest) {
        return false;
      }
    }
    return false;
  }

  size_t offset = 0;
  while (offset + 2U <= limit) {
    const uint32_t pc = base + static_cast<uint32_t>(offset);
    if (in_literal(pc)) {
      offset += 2U;
      continue;
    }
    uint16_t hw1 = 0;
    memcpy(&hw1, code + offset, sizeof(hw1));
    if (thumb_is_it(hw1)) {
      const unsigned it_count = thumb_it_count(hw1);
      size_t span = 2U;
      for (unsigned k = 0; k < it_count; ++k) {
        if (offset + span + 2U > limit)
          return false;
        uint16_t inner_hw1 = 0;
        memcpy(&inner_hw1, code + offset + span, sizeof(inner_hw1));
        const uint32_t inner_pc = base + static_cast<uint32_t>(offset + span);
        Instruction inner{};
        size_t width = 2U;
        if (thumb_is_32bit(inner_hw1)) {
          if (offset + span + 4U > limit)
            return false;
          uint16_t inner_hw2 = 0;
          memcpy(&inner_hw2, code + offset + span + 2U, sizeof(inner_hw2));
          width = 4U;
          if (detail::decode_t32(inner_hw1, inner_hw2, inner_pc, &inner) ==
              Error::None) {
            record_lit(inner, inner_pc);
            const bool is_direct = inner.kind == Kind::ThumbBranch32Cond ||
                                   inner.kind == Kind::ThumbBranch32 ||
                                   inner.kind == Kind::ThumbBl32 ||
                                   inner.kind == Kind::ThumbBlx32;
            if (is_direct) {
              const uint32_t raw_target = inner.target & ~uint32_t{1};
              if (offset >= covered_size && raw_target > base &&
                  raw_target < base + static_cast<uint32_t>(covered_size))
                return true;
              if ((inner.kind == Kind::ThumbBranch32Cond ||
                   inner.kind == Kind::ThumbBranch32) &&
                  raw_target > furthest &&
                  raw_target < base + static_cast<uint32_t>(limit))
                furthest = raw_target;
            }
          }
        } else if (detail::decode_t16(inner_hw1, inner_pc, &inner) ==
                   Error::None) {
          record_lit(inner, inner_pc);
          const bool is_direct = inner.kind == Kind::ThumbBranch16Cond ||
                                 inner.kind == Kind::ThumbBranch16 ||
                                 inner.kind == Kind::ThumbCbz;
          if (is_direct) {
            const uint32_t raw_target = inner.target & ~uint32_t{1};
            if (offset >= covered_size && raw_target > base &&
                raw_target < base + static_cast<uint32_t>(covered_size))
              return true;
            if (raw_target > furthest &&
                raw_target < base + static_cast<uint32_t>(limit))
              furthest = raw_target;
          }
        }
        span += width;
      }
      offset += span;
      continue;
    }

    Instruction insn{};
    size_t width = 2U;
    Error dec = Error::Unsupported;
    if (thumb_is_32bit(hw1)) {
      if (offset + 4U > limit)
        return false;
      uint16_t hw2 = 0;
      memcpy(&hw2, code + offset + 2U, sizeof(hw2));
      width = 4U;
      dec = detail::decode_t32(hw1, hw2, pc, &insn);
    } else {
      dec = detail::decode_t16(hw1, pc, &insn);
    }
    if (dec == Error::None) {
      record_lit(insn, pc);
      if (insn.kind == Kind::ThumbTb32 && offset >= covered_size)
        return false; // Jump table bytes follow immediately after TBB/TBH.
      const bool is_direct =
          insn.kind == Kind::ThumbBranch16Cond ||
          insn.kind == Kind::ThumbBranch16 || insn.kind == Kind::ThumbCbz ||
          insn.kind == Kind::ThumbBranch32Cond ||
          insn.kind == Kind::ThumbBranch32 || insn.kind == Kind::ThumbBl32 ||
          insn.kind == Kind::ThumbBlx32;
      if (is_direct) {
        const uint32_t raw_target = insn.target & ~uint32_t{1};
        if (offset >= covered_size && raw_target > base &&
            raw_target < base + static_cast<uint32_t>(covered_size))
          return true;
        if (insn.kind != Kind::ThumbBl32 && insn.kind != Kind::ThumbBlx32 &&
            raw_target > furthest &&
            raw_target < base + static_cast<uint32_t>(limit))
          furthest = raw_target;
      }
      if (offset >= covered_size && insn.is_terminal && pc + width >= furthest)
        return false;
    } else if (offset >= covered_size &&
               ((hw1 & 0xff00U) == 0xbe00U || (hw1 & 0xff00U) == 0xde00U) &&
               pc + width >= furthest) {
      return false;
    }
    offset += width;
  }
  return false;
}

} // namespace yuki::ihook::arm32
