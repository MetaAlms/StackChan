#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace webrtc_media {

// Newlib Nano cannot format 64-bit integers. Keep their decimal spelling on
// the stack and pass c_str() to the logger through %s instead.
class DecimalLogValue {
public:
    DecimalLogValue(uint64_t magnitude, bool negative) {
        do {
            digits_[--begin_] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (negative) digits_[--begin_] = '-';
    }

    const char* c_str() const { return digits_.data() + begin_; }

private:
    std::array<char, 22> digits_{};
    size_t begin_ = digits_.size() - 1;
};

inline DecimalLogValue SignedDecimal(int64_t value) {
    // Negating INT64_MIN directly overflows. Shift first, then add unsigned.
    const uint64_t magnitude = value < 0
        ? static_cast<uint64_t>(-(value + 1)) + 1
        : static_cast<uint64_t>(value);
    return DecimalLogValue(magnitude, value < 0);
}

inline DecimalLogValue UnsignedDecimal(uint64_t value) {
    return DecimalLogValue(value, false);
}

}  // namespace webrtc_media
