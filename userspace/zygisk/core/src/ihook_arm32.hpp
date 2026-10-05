#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace yuki::ihook::arm32 {

enum class Error : uint8_t {
  None,
  InvalidInput,
  Unsupported,
  Capacity,
};

enum class Kind : uint8_t {
  Copy,
  ArmBranch,
  ThumbBranch,
};

struct Instruction {
  uint8_t offset = 0;
  uint8_t width = 0;
  Kind kind = Kind::Copy;
};

struct Plan {
  Instruction instructions[6] = {};
  uint8_t count = 0;
  uint8_t covered_size = 0;
  uint8_t copied_size = 0;
  Error error = Error::None;
};

[[nodiscard]] inline bool thumb_is_32bit(uint16_t instruction) {
  const uint16_t prefix = instruction >> 11;
  return prefix == 0x1d || prefix == 0x1e || prefix == 0x1f;
}

[[nodiscard]] inline bool thumb_is_wide_branch(const uint16_t *instruction) {
  return (instruction[0] & 0xf800U) == 0xf000U &&
         (instruction[1] & 0xd000U) == 0x9000U;
}

[[nodiscard]] inline bool thumb_is_pcrel(const uint16_t *instruction,
                                         bool wide) {
  const uint16_t first = instruction[0];
  if (!wide) {
    if ((first & 0xff00U) == 0xbf00U && (first & 0xfU) != 0)
      return true;
    if ((first & 0xf800U) == 0x4800U || (first & 0xf800U) == 0xa000U ||
        (first & 0xf000U) == 0xd000U || (first & 0xf800U) == 0xe000U ||
        (first & 0xf500U) == 0xb100U || (first & 0xff00U) == 0xbd00U)
      return true;
    if ((first & 0xfc00U) == 0x4400U) {
      const unsigned rm = (first >> 3) & 0xfU;
      const unsigned rd = (first & 7U) | ((first >> 4) & 8U);
      return rm == 15 || rd == 15;
    }
    return false;
  }
  const uint16_t second = instruction[1];
  if ((first & 0xfe00U) == 0xea00U &&
      ((first & 0xfU) == 15 || (second & 0xfU) == 15 ||
       ((second >> 8) & 0xfU) == 15))
    return true;
  if ((first & 0xfU) == 15 &&
      ((first & 0xff00U) == 0xed00U || (first & 0xf800U) == 0xf800U))
    return true;
  if ((first & 0xff7fU) == 0xf85fU || (first & 0xfbffU) == 0xf20fU ||
      (first & 0xfbffU) == 0xf2afU ||
      ((first & 0xf800U) == 0xf000U && (second & 0x8000U) != 0) ||
      ((first & 0xfff0U) == 0xe8d0U && (second & 0xfff0U) == 0xf000U) ||
      ((first & 0xfff0U) == 0xe8b0U && (second & 0x8000U) != 0))
    return true;
  return false;
}

[[nodiscard]] inline bool thumb_is_stack_push(const uint16_t *instruction) {
  const uint16_t first = instruction[0];
  const uint16_t second = instruction[1];
  if (first == 0xe92dU)
    return (second & 0xa000U) == 0 && second != 0 &&
           (second & (second - 1U)) != 0;
  if ((first & 0xffbfU) != 0xed2dU || (second & 0x0e00U) != 0x0a00U)
    return false;
  const bool doubles = (second & 0x0100U) != 0;
  const unsigned words = second & 0xffU;
  const unsigned high = (first >> 6) & 1U;
  const unsigned vd = second >> 12;
  const unsigned start = doubles ? high * 16U + vd : vd * 2U + high;
  return words != 0 && (!doubles || (words & 1U) == 0) &&
         start + (doubles ? words / 2U : words) <= 32U;
}

[[nodiscard]] inline bool arm_is_pcrel(uint32_t instruction) {
  if ((instruction >> 28) == 0xfU ||
      ((instruction & 0x0e000000U) == 0x08000000U &&
       (instruction & 0x8000U) != 0))
    return true;
  if ((instruction & 0x0ffffff0U) == 0x012fff10U ||
      (instruction & 0x0ffffff0U) == 0x012fff30U ||
      (instruction & 0x0e000000U) == 0x0a000000U ||
      (instruction & 0x0f000000U) == 0x0f000000U)
    return true;
  const unsigned rn = (instruction >> 16) & 0xfU;
  const unsigned group = instruction & 0x0c000000U;
  if (group == 0 || group == 0x04000000U) {
    if (rn == 15 || ((instruction >> 12) & 0xfU) == 15)
      return true;
    if ((instruction & 0x02000000U) == 0 && (instruction & 0xfU) == 15)
      return true;
    if (group == 0 && (instruction & 0x02000010U) == 0x10U &&
        ((instruction >> 8) & 0xfU) == 15)
      return true;
  }
  return group == 0x0c000000U;
}

[[nodiscard]] inline bool arm_is_branch(uint32_t instruction) {
  return (instruction >> 28) != 0xfU &&
         (instruction & 0x0f000000U) == 0x0a000000U;
}

[[nodiscard]] inline Error analyze(const uint8_t *bytes, size_t available,
                                   uintptr_t address, size_t minimum,
                                   bool thumb, Plan *out) {
  if (out == nullptr || bytes == nullptr || minimum == 0 || minimum > 12 ||
      (!thumb && (address & 3U) != 0) || (thumb && (address & 1U) != 0))
    return Error::InvalidInput;
  *out = Plan{};
  out->covered_size = static_cast<uint8_t>(minimum);
  size_t copied = 0;
  while (copied < minimum) {
    if (out->count == 6 || copied + 2 > available)
      return out->error = Error::Capacity;
    Instruction insn{};
    insn.offset = static_cast<uint8_t>(copied);
    if (!thumb) {
      if (copied + 4 > available)
        return out->error = Error::Capacity;
      uint32_t word = 0;
      memcpy(&word, bytes + copied, sizeof(word));
      if ((arm_is_pcrel(word) && !arm_is_branch(word)) ||
          (arm_is_branch(word) && (word & 0x01000000U) != 0))
        return out->error = Error::Unsupported;
      if (arm_is_branch(word))
        insn.kind = Kind::ArmBranch;
      else if ((word & 0x0e000000U) > 0x08000000U)
        return out->error = Error::Unsupported;
      insn.width = 4;
    } else {
      uint16_t first = 0;
      memcpy(&first, bytes + copied, sizeof(first));
      if (thumb_is_32bit(first)) {
        if (copied + 4 > available)
          return out->error = Error::Capacity;
        uint16_t pair[2] = {first, 0};
        memcpy(&pair[1], bytes + copied + 2, sizeof(uint16_t));
        if (thumb_is_wide_branch(pair))
          insn.kind = Kind::ThumbBranch;
        else {
          const bool immediate =
              ((first & 0xff00U) == 0xf100U || (first & 0xff00U) == 0xf200U) &&
              ((pair[1] >> 8) & 0xfU) != 15U;
          if (thumb_is_pcrel(pair, true) ||
              (!thumb_is_stack_push(pair) && !immediate))
            return out->error = Error::Unsupported;
        }
        insn.width = 4;
      } else {
        if (thumb_is_pcrel(&first, false) || (first & 0xff00U) == 0xbe00U)
          return out->error = Error::Unsupported;
        insn.width = 2;
      }
    }
    out->instructions[out->count++] = insn;
    copied += insn.width;
  }
  out->copied_size = static_cast<uint8_t>(copied);
  return Error::None;
}

} // namespace yuki::ihook::arm32
