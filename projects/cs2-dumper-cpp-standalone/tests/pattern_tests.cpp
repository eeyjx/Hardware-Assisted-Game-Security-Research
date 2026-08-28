#include "cs2dumper/pattern.hpp"
#include "cs2dumper/pe.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename T>
void put(std::vector<std::uint8_t>& b, std::size_t off, T value) {
    assert(off + sizeof(T) <= b.size());
    std::memcpy(b.data() + off, &value, sizeof(T));
}

std::vector<std::uint8_t> make_image() {
    std::vector<std::uint8_t> b(0x1000, 0xCC);
    b[0] = 'M'; b[1] = 'Z';
    put<std::uint32_t>(b, 0x3C, 0x80);
    put<std::uint32_t>(b, 0x80, 0x00004550);
    // IMAGE_FILE_HEADER at 0x84
    put<std::uint16_t>(b, 0x84 + 0, 0x8664);
    put<std::uint16_t>(b, 0x84 + 2, 1);
    put<std::uint16_t>(b, 0x84 + 16, 0xF0);
    // PE32+ optional header at 0x98
    put<std::uint16_t>(b, 0x98, 0x20B);
    put<std::uint64_t>(b, 0x98 + 24, 0x180000000ull);
    // One .text section at 0x188.
    const std::size_t sh = 0x98 + 0xF0;
    std::memcpy(b.data() + sh, ".text", 5);
    put<std::uint32_t>(b, sh + 8, 0x500);   // virtual size
    put<std::uint32_t>(b, sh + 12, 0x200);  // virtual address
    put<std::uint32_t>(b, sh + 16, 0x500);  // raw size
    put<std::uint32_t>(b, sh + 36, 0x60000020u); // code|execute|read
    return b;
}

} // namespace

int main() {
    using namespace cs2dumper;

    // Ensure every built-in pattern syntax used by the analyzer parses.
    const std::vector<std::string> all_patterns = {
        "488b15${'} 4885d2 74? 488b02 4885c0",
        "488905${'} 0f57c0 0f1105", "f2420f108428u4", "48890d${'} e9${} cc",
        "488b1d${'} 48891d[4] 4c63b3", "ff81u4 4885d2", "f6c1010f85${} 4c8b05${'} 4d85",
        "488915${'} 488942", "488b05${'} c3 cccccccccccccccc 8b41", "488b05${'} 4189be",
        "488b1d${'} 4532f6", "488d05${'} c3 cccccccccccccccc 405356 4154",
        "4c39b6u4 74? 4488be", "488d0d${[8]'} 660f6ecd",
        "488d0d${'} 48c1e006", "488905${'} 488bc8 4885c0",
        "488b15${'} 488b5c24? ffc0 8905${} 488bc6 488934ea 80be",
        "8905${'} 488d0d${} ff15${} 488b0d", "48893d${'} ff87",
        "8b81u4 c3 cccccccccccccccccc 8b81${} c3 cccccccccccccccccc 83b9", "4c8db7u4 4c897c24",
        "0fb681u4 c3 cccccccccccccccc 0fb681${} c3 cccccccccccccccc 4883ec",
        "428b94d3u4 5b 49ffe3 32c0 5b c3 cccccccccccccccc 4053",
        "8b81u4 c3????????? 8b81[4] c3????????? 8b81", "8b81u4 c3 cccccccccccccccccc 83b9",
        "448b81u4 488d0d", "8b05${'} 8903", "8b05${'} 8907", "488905${'} 33c0",
        "488d0d${'} ff90", "488d0d${'} e8${} 488b0d${} [3] 4c8b82", "0f1147u1 0f104e? 0f118f",
        "4c8d35${'} 0f2845", "488935${'} 4885f6", "8983u4 40b7",
        "8905${'} 488d0d${} ff15${}", "48893d${'} 488d15"
    };
    for (const auto& text : all_patterns) Pattern p(text);

    auto image = make_image();
    // Match at RVA 0x220: 48 8B 15 <rel32 to 0x350> 48 85 D2 74 xx 48 8B 02 48 85 C0
    std::size_t p = 0x220;
    const std::uint8_t prefix[] = {0x48, 0x8B, 0x15};
    std::memcpy(image.data() + p, prefix, sizeof(prefix));
    const std::int32_t disp = static_cast<std::int32_t>(0x350 - (0x223 + 4));
    put<std::int32_t>(image, 0x223, disp);
    const std::uint8_t suffix[] = {0x48,0x85,0xD2,0x74,0x11,0x48,0x8B,0x02,0x48,0x85,0xC0};
    std::memcpy(image.data() + 0x227, suffix, sizeof(suffix));

    PeImage pe(image);
    assert(pe.valid());
    PatternScanner scanner(pe);
    const auto match = scanner.find_unique_code(Pattern("488b15${'} 4885d2 74? 488b02 4885c0"));
    assert(match.has_value());
    assert(match->saves.size() == 2);
    assert(match->saves[0] == 0x220);
    assert(match->saves[1] == 0x350);

    // u4 captures the immediate value, rather than an RVA.
    const std::uint8_t u4prefix[] = {0x4C,0x39,0xB6};
    std::memcpy(image.data() + 0x280, u4prefix, sizeof(u4prefix));
    put<std::uint32_t>(image, 0x283, 0x12345678u);
    const std::uint8_t u4suffix[] = {0x74,0x7F,0x44,0x88,0xBE};
    std::memcpy(image.data() + 0x287, u4suffix, sizeof(u4suffix));
    PeImage pe2(image);
    PatternScanner scanner2(pe2);
    const auto u4 = scanner2.find_unique_code(Pattern("4c39b6u4 74? 4488be"));
    assert(u4.has_value());
    assert(u4->saves.size() == 2 && u4->saves[1] == 0x12345678u);

    std::cout << "pattern tests passed\n";
    return 0;
}
