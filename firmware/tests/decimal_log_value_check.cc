#include "decimal_log_value.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
unsigned checks = 0;
void Require(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) Require(bool(expression), #expression, __LINE__)
using webrtc_media::SignedDecimal;
using webrtc_media::UnsignedDecimal;

void SignedBoundaries()
{
    struct Case { int64_t value; const char* expected; };
    const Case cases[] = {
        {0, "0"}, {-1, "-1"}, {1, "1"}, {9, "9"}, {-9, "-9"},
        {10, "10"}, {-10, "-10"}, {99, "99"}, {-99, "-99"},
        {100, "100"}, {-100, "-100"},
        {30000000, "30000000"}, {180000000, "180000000"},
        {2147483647, "2147483647"}, {2147483648LL, "2147483648"},
        {-2147483648LL, "-2147483648"}, {-2147483649LL, "-2147483649"},
        {4294967295LL, "4294967295"}, {4294967296LL, "4294967296"},
        {-4294967296LL, "-4294967296"},
        {1000000000000000000LL, "1000000000000000000"},
        {std::numeric_limits<int64_t>::max(), "9223372036854775807"},
        {std::numeric_limits<int64_t>::min(), "-9223372036854775808"},
    };
    for (const auto& c : cases) CHECK(std::strcmp(SignedDecimal(c.value).c_str(), c.expected) == 0);
}

void UnsignedBoundaries()
{
    struct Case { uint64_t value; const char* expected; };
    const Case cases[] = {
        {0, "0"}, {1, "1"}, {9, "9"}, {10, "10"}, {99, "99"}, {100, "100"},
        {4294967295ULL, "4294967295"}, {4294967296ULL, "4294967296"},
        {9223372036854775807ULL, "9223372036854775807"},
        {9223372036854775808ULL, "9223372036854775808"},
        {10000000000000000000ULL, "10000000000000000000"},
        {std::numeric_limits<uint64_t>::max(), "18446744073709551615"},
    };
    for (const auto& c : cases) CHECK(std::strcmp(UnsignedDecimal(c.value).c_str(), c.expected) == 0);
    auto copied = UnsignedDecimal(std::numeric_limits<uint64_t>::max());
    const auto second = copied;
    copied = UnsignedDecimal(0);
    CHECK(std::strcmp(copied.c_str(), "0") == 0);
    CHECK(std::strcmp(second.c_str(), "18446744073709551615") == 0);
}

void LoggerArgumentAlignment()
{
    char output[256];
    const int written = std::snprintf(output, sizeof(output),
        "before=%u elapsed_ms=%s config=%u samples=%s after=%u t0_us=%s last=%u",
        17u, SignedDecimal(-1).c_str(), 1u,
        UnsignedDecimal(std::numeric_limits<uint64_t>::max()).c_str(),
        29u, SignedDecimal(4294967296LL).c_str(), 31u);
    const char* expected = "before=17 elapsed_ms=-1 config=1 samples=18446744073709551615 after=29 t0_us=4294967296 last=31";
    CHECK(written == static_cast<int>(std::strlen(expected)));
    CHECK(std::strcmp(output, expected) == 0);
    const char* bounds = "signed=-9223372036854775808 unsigned=0 next=4294967295";
    CHECK(std::snprintf(output, sizeof(output), "signed=%s unsigned=%s next=%u",
        SignedDecimal(std::numeric_limits<int64_t>::min()).c_str(),
        UnsignedDecimal(0).c_str(), 4294967295u) == static_cast<int>(std::strlen(bounds)));
    CHECK(std::strcmp(output, bounds) == 0);
}
}  // namespace

int main()
{
    static_assert(std::is_trivially_copyable<webrtc_media::DecimalLogValue>::value,
        "The decimal value must own only fixed storage and an offset");
    SignedBoundaries();
    UnsignedBoundaries();
    LoggerArgumentAlignment();
    std::printf("decimal_log_value_check: PASS (%u checks)\n", checks);
}
