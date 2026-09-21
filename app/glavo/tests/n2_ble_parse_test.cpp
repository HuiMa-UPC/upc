#include "n2_ble_parse.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using upc::glavo::N2BleParsed;
using upc::glavo::N2BleState;
using upc::glavo::parse_n2_ble_ascii;

static int g_fail = 0;

static void expect(bool ok, const char* name)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++g_fail;
    }
}

int main()
{
    N2BleParsed p{};
    expect(parse_n2_ble_ascii("N2=99.50% STATE=RUN seq=1", &p), "run parse");
    expect(p.n2_valid && p.n2_percent > 99.4f && p.n2_percent < 99.6f, "pct");
    expect(p.state == N2BleState::Run && p.seq == 1, "run state seq");

    expect(parse_n2_ble_ascii("N2=warming STATE=WARMUP seq=0", &p), "warm parse");
    expect(!p.n2_valid && p.state == N2BleState::Warmup && p.seq == 0, "warm");

    expect(parse_n2_ble_ascii("N2=waiting", &p), "wait parse");
    expect(!p.n2_valid && !p.has_seq, "wait");

    expect(parse_n2_ble_ascii("N2=100.00% STATE=WARMUP seq=185", &p), "100 warmup");
    expect(p.n2_valid && p.n2_percent > 99.9f && p.state == N2BleState::Warmup
           && p.seq == 185, "100 warmup fields");

    expect(!parse_n2_ble_ascii("hello", &p), "reject");
    expect(!parse_n2_ble_ascii("", &p), "empty");

    if (g_fail) {
        std::fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    std::puts("n2_ble_parse_test ok");
    return 0;
}
