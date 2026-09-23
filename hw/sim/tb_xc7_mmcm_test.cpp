// tb_xc7_mmcm_test — verify xc7_mmcm_compute's divider search.
//
// Checks properties rather than particular dividers, so any preference
// order among legal solutions passes: a valid config must sit inside every
// datasheet limit, be exact in whole hertz, and agree with itself
// (fPFD = in/D, fVCO = in*M/D, CLKOUT0 = fVCO/O0). Targets no legal set
// reaches exactly must come back invalid. The chosen dividers are printed,
// so a change of preference is visible in the log.
#include "Vxc7_mmcm_test.h"
#include "verilated.h"
#include <cstdio>
#include <vector>

static int checks = 0, fails = 0;
static void chk(const char* name, long long got, long long exp) {
    checks++;
    if (got != exp) {
        fails++;
        printf("  FAIL %s: got %lld, expected %lld\n", name, got, exp);
    }
}
static void chk_true(const char* name, bool ok) { chk(name, ok, 1); }

static const long long IN_HZ = 100000000LL;

// Reference search: every legal, exact config for in_hz -> target_hz, by
// the rules xc7_mmcm_pkg documents, restated independently so the test
// does not trust the code it checks. It turns a bare "valid: got 0" into
// concrete configs to trace, and proves the rejected targets really have
// none.
struct Cfg { long long d, m8, o8, vco; };
static std::vector<Cfg> legal_configs(long long in_hz, long long target_hz) {
    std::vector<Cfg> v;
    if (target_hz <= 0 || target_hz > 464000000LL)            // FMAX_BUFG
        return v;
    for (long long d = 1; d <= 106; d++) {
        if (in_hz % d != 0)
            continue;                                         // fPFD exact
        long long pfd = in_hz / d;
        if (pfd < 10000000LL || pfd > 450000000LL)
            continue;
        for (long long m8 = 16; m8 <= 512; m8++) {
            if ((in_hz * m8) % (8 * d) != 0)
                continue;                                     // fVCO exact
            long long vco = in_hz * m8 / (8 * d);
            if (vco < 600000000LL || vco > 1200000000LL)
                continue;
            if ((in_hz * m8) % (d * target_hz) != 0)
                continue;                                     // CLKOUT0 exact
            long long o8 = in_hz * m8 / (d * target_hz);
            if (o8 < 8 || o8 > 1024 || (o8 > 8 && o8 < 16))
                continue;
            v.push_back({d, m8, o8, vco});
        }
    }
    return v;
}

static void show_legal(const char* tag, const std::vector<Cfg>& legal) {
    printf("  %s: %zu legal exact configs exist, e.g.:\n", tag, legal.size());
    for (size_t i = 0; i < legal.size() && i < 4; i++)
        printf("      D=%lld M=%lld/8 O0=%lld/8  fVCO=%lld\n",
               legal[i].d, legal[i].m8, legal[i].o8, legal[i].vco);
}

// Every property a valid config must have, whatever the search preferred.
static void check_config(const char* tag, long long target, bool valid,
                         long long d, long long m8, long long o8,
                         long long pfd, long long vco, long long out) {
    char n[64];
    std::vector<Cfg> legal = legal_configs(IN_HZ, target);
    snprintf(n, sizeof n, "%s.valid", tag); chk(n, valid, 1);
    if (!valid) {
        show_legal(tag, legal);
        return;
    }
    printf("  %s: D=%lld M=%lld/8 O0=%lld/8  fPFD=%lld fVCO=%lld out=%lld\n",
           tag, d, m8, o8, pfd, vco, out);
    snprintf(n, sizeof n, "%s.D range", tag);      chk_true(n, d >= 1 && d <= 106);
    snprintf(n, sizeof n, "%s.M range", tag);      chk_true(n, m8 >= 16 && m8 <= 512);
    snprintf(n, sizeof n, "%s.O0 range", tag);     chk_true(n, o8 >= 8 && o8 <= 1024);
    snprintf(n, sizeof n, "%s.O0 not in (1,2)", tag); chk_true(n, !(o8 > 8 && o8 < 16));
    snprintf(n, sizeof n, "%s.fPFD = in/D", tag);  chk(n, pfd * d, IN_HZ);
    snprintf(n, sizeof n, "%s.fPFD band", tag);    chk_true(n, pfd >= 10000000LL && pfd <= 450000000LL);
    snprintf(n, sizeof n, "%s.fVCO = in*M/D", tag); chk(n, vco * 8 * d, IN_HZ * m8);
    snprintf(n, sizeof n, "%s.fVCO band", tag);    chk_true(n, vco >= 600000000LL && vco <= 1200000000LL);
    snprintf(n, sizeof n, "%s.out = fVCO/O0", tag); chk(n, out * o8, vco * 8);
    snprintf(n, sizeof n, "%s.out exact", tag);    chk(n, out, target);
    bool known = false;
    for (const Cfg& c : legal)
        known = known || (c.d == d && c.m8 == m8 && c.o8 == o8);
    snprintf(n, sizeof n, "%s.in reference set", tag); chk_true(n, known);
}

// A rejected target: the search must refuse it, and the reference must
// agree there is nothing to find (otherwise the test itself is wrong).
static void check_rejected(const char* tag, long long target, bool valid) {
    char n[64];
    std::vector<Cfg> legal = legal_configs(IN_HZ, target);
    snprintf(n, sizeof n, "%s: reference finds none", tag);
    chk(n, (long long)legal.size(), 0);
    if (!legal.empty())
        show_legal(tag, legal);
    snprintf(n, sizeof n, "%s: rejected", tag);
    chk(n, valid, 0);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vxc7_mmcm_test* d = new Vxc7_mmcm_test;
    d->eval();

    check_config("37.5M", 37500000LL, d->o37_valid, d->o37_d, d->o37_m8, d->o37_o8,
                 d->o37_pfd, d->o37_vco, d->o37_out);
    check_config("100M", 100000000LL, d->o100_valid, d->o100_d, d->o100_m8, d->o100_o8,
                 d->o100_pfd, d->o100_vco, d->o100_out);

    check_rejected("37.49M (inexact)",      37490000LL,  d->inexact_valid);
    check_rejected("4M (below 600M/128)",   4000000LL,   d->too_slow_valid);
    check_rejected("500M (above BUFG)",     500000000LL, d->too_fast_valid);

    chk("aux 750M/25M",   d->aux_750_25,   30);
    chk("aux 750M/125M",  d->aux_750_125,  6);
    chk("aux 600M/125M",  d->aux_600_125,  0);
    chk("aux 1000M/500M", d->aux_1000_500, 0);

    delete d;
    if (fails) { printf("FAIL: %d/%d checks failed\n", fails, checks); return 1; }
    printf("PASS: %d/%d checks passed\n", checks, checks);
    return 0;
}
