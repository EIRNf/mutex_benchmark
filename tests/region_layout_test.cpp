// Correctness tests for lib/utils/region_layout.hpp: verifies that reserved
// sub-spans never overlap, total_size() matches the sum of reservations
// (respecting alignment/striding), the cache-line-strided variant uses
// exactly std::hardware_destructive_interference_size, and an end-to-end
// alloc/fill/read-back round trip doesn't cross-contaminate between regions.

#include "region_layout.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static int g_failures = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL  %-24s %s\n", __func__, msg);    \
            g_failures++;                                 \
        } else {                                           \
            printf("OK    %-24s %s\n", __func__, msg);    \
        }                                                  \
    } while (0)

void test_layout_shape_and_size() {
    RegionLayout layout;

    auto scalar  = layout.reserve<int>();                  // 4 bytes @ offset 0
    auto arr     = layout.reserve_array<int>(5);            // 5*4=20 bytes, 4-aligned
    auto strided = layout.reserve_strided_array<char>(3);   // 3 cache lines
    auto bytes   = layout.reserve_bytes(16);                // 16 raw bytes, max_align_t-aligned

    constexpr std::size_t CL = std::hardware_destructive_interference_size;

    CHECK(scalar.offset == 0, "scalar reserved at offset 0");
    CHECK(arr.offset == sizeof(int), "array follows scalar with no padding (both 4-aligned)");
    CHECK(strided.offset % CL == 0, "strided array starts on a cache-line boundary");
    CHECK(strided.offset >= arr.offset + sizeof(int) * 5, "strided array starts after the plain array");
    CHECK(bytes.offset >= strided.offset + CL * 3, "raw bytes reservation starts after the strided array");
    CHECK(bytes.offset % alignof(std::max_align_t) == 0, "raw bytes reservation is max_align_t-aligned");
    CHECK(layout.total_size() == bytes.offset + 16, "total_size() matches the last reservation's end exactly");
}

void test_round_trip_no_overlap() {
    RegionLayout layout;

    auto a = layout.reserve<uint64_t>();
    auto b = layout.reserve_array<uint32_t>(4);
    auto c = layout.reserve_strided_array<uint8_t>(6);
    auto d = layout.reserve_bytes(9);

    std::size_t total = layout.total_size();
    char* base = (char*)malloc(total);
    memset(base, 0, total);

    uint64_t* a_ptr = RegionLayout::resolve(a, base);
    uint32_t* b_ptr = RegionLayout::resolve(b, base);
    uint8_t*  d_ptr = (uint8_t*)RegionLayout::resolve(d, base);

    *a_ptr = 0xAAAAAAAAAAAAAAAAull;
    for (int i = 0; i < 4; i++) b_ptr[i] = 0xB0B0B0B0u + (uint32_t)i;
    for (std::size_t i = 0; i < 6; i++) {
        *RegionLayout::resolve_strided(c, base, i) = (uint8_t)(0xC0 + i);
    }
    memset(d_ptr, 0xDD, 9);

    bool ok = true;
    ok = ok && (*a_ptr == 0xAAAAAAAAAAAAAAAAull);
    for (int i = 0; i < 4; i++) ok = ok && (b_ptr[i] == 0xB0B0B0B0u + (uint32_t)i);
    for (std::size_t i = 0; i < 6; i++) {
        ok = ok && (*RegionLayout::resolve_strided(c, base, i) == (uint8_t)(0xC0 + i));
    }
    for (int i = 0; i < 9; i++) ok = ok && (d_ptr[i] == 0xDD);

    CHECK(ok, "all four regions read back exactly what was written, no cross-contamination");

    free(base);
}

void test_strided_spacing() {
    RegionLayout layout;
    auto nodes = layout.reserve_strided_array<int>(4);

    constexpr std::size_t CL = std::hardware_destructive_interference_size;
    CHECK(nodes.stride == CL, "stride equals std::hardware_destructive_interference_size");

    char* base = (char*)malloc(layout.total_size());
    bool all_spaced_correctly = true;
    for (std::size_t i = 0; i + 1 < 4; i++) {
        auto* p0 = RegionLayout::resolve_strided(nodes, base, i);
        auto* p1 = RegionLayout::resolve_strided(nodes, base, i + 1);
        std::size_t diff = (char*)p1 - (char*)p0;
        all_spaced_correctly = all_spaced_correctly && (diff == CL);
    }
    CHECK(all_spaced_correctly, "consecutive strided elements are exactly one cache line apart");
    free(base);
}

int main() {
    test_layout_shape_and_size();
    test_round_trip_no_overlap();
    test_strided_spacing();

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
