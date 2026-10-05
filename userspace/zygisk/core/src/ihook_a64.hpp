#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace yuki::ihook::a64 {

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
  }
  return "unknown";
}

enum class Kind : uint8_t {
  Copy,
  Adr,
  Adrp,
  Branch,
  Call,
  Conditional,
  Compare,
  Test,
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
};

struct Plan {
  Instruction instructions[2] = {};
  uint8_t offsets[2] = {};
  uint8_t lengths[2] = {};
  uint8_t code_size = 0;
  uint8_t covered_size = 8;
  uint8_t copied_size = 8;
  Error error = Error::None;
};

[[nodiscard]] inline int64_t extend(uint32_t value, unsigned bits) {
  return static_cast<int64_t>(static_cast<int32_t>(value << (32 - bits)) >>
                              (32 - bits));
}

[[nodiscard]] inline bool add_signed(uintptr_t base, int64_t delta,
                                     uintptr_t *result) {
  if (delta >= 0) {
    if (base > UINTPTR_MAX - static_cast<uint64_t>(delta))
      return false;
    *result = base + static_cast<uint64_t>(delta);
  } else {
    const uint64_t magnitude = static_cast<uint64_t>(-(delta + 1)) + 1;
    if (base < magnitude)
      return false;
    *result = base - magnitude;
  }
  return true;
}

[[nodiscard]] inline bool immediate(uintptr_t from, uintptr_t to, unsigned bits,
                                    int64_t *value) {
  if (value == nullptr || bits == 0 || bits > 26 || ((from | to) & 3U) != 0)
    return false;
  const uintptr_t limit = uintptr_t{1} << (bits + 1);
  const bool backward = to < from;
  const uintptr_t distance = backward ? from - to : to - from;
  if (backward ? distance > limit : distance >= limit)
    return false;
  const int64_t delta = static_cast<int64_t>(distance);
  *value = (backward ? -delta : delta) / 4;
  return true;
}

[[nodiscard]] inline Error decode(uint32_t bits, uintptr_t address,
                                  Instruction *out) {
  if (out == nullptr || (address & 3) != 0)
    return Error::InvalidInput;
  *out = {bits, Kind::Copy, 0, 0};
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
    imm_bits = 26;
    shift = 0;
  } else if ((bits & 0xff000010U) == 0x54000000U && (bits & 0xfU) < 14) {
    out->kind = Kind::Conditional;
    imm_bits = 19;
  } else if ((bits & 0x7e000000U) == 0x34000000U) {
    out->kind = Kind::Compare;
    imm_bits = 19;
  } else if ((bits & 0x7e000000U) == 0x36000000U) {
    out->kind = Kind::Test;
    imm_bits = 14;
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
    // Only known position-independent instructions can be copied unchanged.
    const bool hint = (bits & 0xfffff01fU) == 0xd503201fU;
    const bool pac = bits == 0xd503233fU || bits == 0xd50323bfU ||
                     bits == 0xd503237fU || bits == 0xd50323ffU;
    const bool stack_pair =
        (bits & 0x3b000000U) == 0x29000000U && ((bits >> 5) & 31U) == 31U;
    const bool stack_mem =
        (bits & 0x3b000000U) == 0x39000000U && ((bits >> 5) & 31U) == 31U;
    const bool sf = (bits & 0x80000000U) != 0;
    const bool add_sub_imm = (bits & 0x1f800000U) == 0x11000000U;
    const bool move_wide = (bits & 0x1f800000U) == 0x12800000U &&
                           ((bits >> 29) & 3U) != 1U &&
                           (sf || (bits & 0x00400000U) == 0);
    const bool logical_reg = (bits & 0x1f000000U) == 0x0a000000U &&
                             (sf || (bits & 0x00008000U) == 0);
    const bool extended = (bits & 0x00200000U) != 0;
    const unsigned shift = (bits >> 22) & 3U;
    const bool add_sub_reg =
        (bits & 0x1f000000U) == 0x0b000000U &&
        (extended ? shift == 0 && ((bits >> 10) & 7U) <= 4U
                  : shift != 3 && (sf || (bits & 0x00008000U) == 0));
    const bool ret = (bits & 0xfffffc1fU) == 0xd65f0000U;
    const bool blr = (bits & 0xfffffc1fU) == 0xd63f0000U;
    if (!hint && !pac && !stack_pair && !stack_mem && !add_sub_imm &&
        !move_wide && !logical_reg && !add_sub_reg && !ret && !blr)
      return Error::Unsupported;
    return Error::None;
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

[[nodiscard]] inline Error analyze(const uint8_t *source, size_t size,
                                   uintptr_t address, Plan *out) {
  if (source == nullptr || out == nullptr || size < 8 || (address & 3) != 0 ||
      address > UINTPTR_MAX - 8)
    return Error::InvalidInput;
  *out = Plan{};
  for (size_t i = 0; i != 2; ++i) {
    uint32_t bits = 0;
    memcpy(&bits, source + i * 4, sizeof(bits));
    const Error result = decode(bits, address + i * 4, &out->instructions[i]);
    if (result != Error::None)
      return out->error = result;
    const auto &insn = out->instructions[i];
    if (is_literal(insn.kind) && insn.width != 0 &&
        (insn.target > UINTPTR_MAX - insn.width ||
         overlaps(insn.target, insn.width, address, 8)))
      return out->error = Error::Overlap;
    if (insn.kind == Kind::Adr && insn.target >= address &&
        insn.target < address + 8)
      return out->error = Error::Overlap;
    if ((insn.kind == Kind::Branch || insn.kind == Kind::Call ||
         insn.kind == Kind::Conditional || insn.kind == Kind::Compare ||
         insn.kind == Kind::Test) &&
        insn.target >= address && insn.target < address + 8 &&
        ((insn.target - address) & 3) != 0)
      return out->error = Error::Unsupported;
  }
  return Error::None;
}

[[nodiscard]] inline uintptr_t mapped_target(const Plan &plan, uintptr_t source,
                                             uintptr_t destination,
                                             uintptr_t target) {
  if (target == source)
    return destination + plan.offsets[0];
  if (target == source + 4)
    return destination + plan.offsets[1];
  return target;
}

[[nodiscard]] inline Error plan(const uint8_t *source, size_t size,
                                uintptr_t source_address, uintptr_t destination,
                                size_t capacity, Plan *out) {
  const Error analyzed = analyze(source, size, source_address, out);
  if (analyzed != Error::None)
    return analyzed;
  if (capacity < 12 || destination > UINTPTR_MAX - capacity)
    return out->error = Error::Capacity;
  out->offsets[0] = 0;
  out->offsets[1] = 4;
  out->lengths[0] = out->lengths[1] = 4;
  for (unsigned pass = 0; pass != 4; ++pass) {
    bool changed = false;
    uint8_t cursor = 0;
    for (size_t i = 0; i != 2; ++i) {
      out->offsets[i] = cursor;
      cursor += out->lengths[i];
    }
    for (size_t i = 0; i != 2; ++i) {
      const auto &insn = out->instructions[i];
      const uintptr_t at = destination + out->offsets[i];
      int64_t value = 0;
      uint8_t length = 4;
      switch (insn.kind) {
      case Kind::Adr: {
        const int64_t delta = static_cast<int64_t>(insn.target - at);
        length =
            delta >= -(int64_t{1} << 20) && delta < (int64_t{1} << 20) ? 4 : 16;
        break;
      }
      case Kind::Adrp: {
        const int64_t pages =
            static_cast<int64_t>((insn.target & ~uintptr_t{4095}) -
                                 (at & ~uintptr_t{4095})) /
            4096;
        length =
            pages >= -(int64_t{1} << 20) && pages < (int64_t{1} << 20) ? 4 : 16;
        break;
      }
      case Kind::Branch:
      case Kind::Call:
        if (!immediate(
                at,
                mapped_target(*out, source_address, destination, insn.target),
                26, &value))
          return out->error = Error::Unreachable;
        break;
      case Kind::Conditional:
      case Kind::Compare:
      case Kind::Test: {
        const unsigned bits = insn.kind == Kind::Test ? 14 : 19;
        const uintptr_t target =
            mapped_target(*out, source_address, destination, insn.target);
        if (!immediate(at, target, bits, &value)) {
          if (!immediate(at + 4, target, 26, &value))
            return out->error = Error::Unreachable;
          length = 8;
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
        length =
            immediate(at, insn.target, 19, &value)
                ? 4
                : (insn.kind <= Kind::LiteralSigned && (insn.bits & 31U) != 31U
                       ? 20
                       : 36);
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
      const size_t used = out->offsets[1] + out->lengths[1] + 4;
      if (used > capacity)
        return out->error = Error::Capacity;
      int64_t back = 0;
      if (!immediate(destination + used - 4, source_address + 8, 26, &back))
        return out->error = Error::Unreachable;
      out->code_size = static_cast<uint8_t>(used);
      return Error::None;
    }
  }
  return out->error = Error::Unreachable;
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
    emit_word(out, offset + part * 4,
              op | (part << 21) |
                  (static_cast<uint32_t>(
                       (static_cast<uint64_t>(target) >> (part * 16)) & 0xffffU)
                   << 5) |
                  rd);
  }
}

[[nodiscard]] inline Error emit(const Plan &plan, uintptr_t source,
                                uintptr_t destination, uint8_t *out,
                                size_t capacity) {
  if (plan.error != Error::None || out == nullptr ||
      plan.code_size > capacity || plan.code_size < 12)
    return Error::Capacity;
  for (size_t i = 0; i != 2; ++i) {
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
      if (length == 16) {
        emit_address(out, offset, rd, insn.target);
        continue;
      }
      const bool page = insn.kind == Kind::Adrp;
      const int64_t delta = static_cast<int64_t>(
          insn.target - (page ? at & ~uintptr_t{4095} : at));
      const uint32_t encoded =
          static_cast<uint32_t>(page ? delta >> 12 : delta) & 0x1fffffU;
      word =
          (word & 0x9f00001fU) | ((encoded & 3U) << 29) | ((encoded >> 2) << 5);
      break;
    }
    case Kind::Branch:
    case Kind::Call:
      word = branch(at, mapped_target(plan, source, destination, insn.target),
                    insn.kind == Kind::Call);
      if (word == 0)
        return Error::Unreachable;
      break;
    case Kind::Conditional:
    case Kind::Compare:
    case Kind::Test: {
      const unsigned bits = insn.kind == Kind::Test ? 14 : 19;
      const uint32_t mask = (uint32_t{1} << bits) - 1U;
      const uintptr_t target =
          mapped_target(plan, source, destination, insn.target);
      if (length == 8) {
        word ^= insn.kind == Kind::Conditional ? 1U : 0x01000000U;
        word = (word & ~(mask << 5)) | (2U << 5);
        emit_word(out, offset, word);
        const uint32_t jump = branch(at + 4, target);
        if (jump == 0)
          return Error::Unreachable;
        emit_word(out, offset + 4, jump);
        continue;
      }
      if (!immediate(at, target, bits, &imm))
        return Error::Unreachable;
      word = (word & ~(mask << 5)) | ((static_cast<uint32_t>(imm) & mask) << 5);
      break;
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
        emit_address(out, offset, rd, insn.target);
        const uint32_t load = insn.kind == Kind::LiteralW        ? 0xb9400000U
                              : insn.kind == Kind::LiteralSigned ? 0xb9800000U
                                                                 : 0xf9400000U;
        emit_word(out, offset + 16, load | (rd << 5) | rd);
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
      emit_word(out, offset + 24, load | (16U << 5) | rd);
      emit_word(out, offset + 28, 0xf94003f0U); // ldr x16, [sp]
      emit_word(out, offset + 32, 0x910043ffU); // add sp, sp, #16
      continue;
    }
    case Kind::Copy:
      break;
    }
    emit_word(out, offset, word);
  }
  const uint32_t back = branch(destination + plan.code_size - 4, source + 8);
  if (back == 0)
    return Error::Unreachable;
  emit_word(out, plan.code_size - 4, back);
  return Error::None;
}

} // namespace yuki::ihook::a64
