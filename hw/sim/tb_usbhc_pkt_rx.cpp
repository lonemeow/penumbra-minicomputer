// Verilator testbench for the Penumbra USB host packet receiver
//
// Plays the PHY side of the UTMI-shaped seam's receive half: opens a packet
// window (i_rx_active), strobes bytes at a programmable pace, optionally
// flags a line error, and closes the window. Buffer writes are shadowed and
// the latched classification checked against expectations built with the
// same CRC reference as the transmit testbench (mirroring
// sw/tools/usb_crc.py). Packets cover good/corrupt CRC, bad PID check
// nibbles, runts, buffer overflow, and byte-per-cycle pacing.
//
// The receiver stores the packet body as it arrives, trailing CRC bytes
// included (the device contract: buffer content beyond RXLEN is not
// meaningful) — so a data packet's expected write count is its post-PID
// byte count clamped at the buffer size, while the content check compares
// only the o_len payload bytes.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Vusbhc_pkt_rx.h"

static const int BUF_BYTES = 64;

// PID nibbles (usb_pkg::usb_pid_e).
static const uint8_t PID_DATA0 = 0x3;
static const uint8_t PID_DATA1 = 0xB;
static const uint8_t PID_ACK   = 0x2;
static const uint8_t PID_NAK   = 0xA;
static const uint8_t PID_STALL = 0xE;

static uint8_t pid_byte(uint8_t pid) {
    return (uint8_t)(((~pid & 0xf) << 4) | (pid & 0xf));
}

// ── CRC reference, step-for-step usb_crc.py ──────────────────────────────

static uint32_t reflect(uint32_t value, int width) {
    uint32_t result = 0;
    for (int i = 0; i < width; i++)
        if (value & (1u << i))
            result |= 1u << (width - 1 - i);
    return result;
}

static uint16_t crc16_remainder(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xffff;
    for (uint8_t byte : data) {
        for (int i = 0; i < 8; i++) {
            int feedback = ((crc >> 15) & 1) ^ ((byte >> i) & 1);
            crc = (uint16_t)(crc << 1);
            if (feedback)
                crc ^= 0x8005;
        }
    }
    return crc;
}

static uint16_t data_crc16(const std::vector<uint8_t>& data) {
    return (uint16_t)reflect(crc16_remainder(data) ^ 0xffff, 16);
}

// On-wire image of a data packet: PID, payload, CRC low byte then high.
static std::vector<uint8_t> data_packet(uint8_t pid,
                                        const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> v{ pid_byte(pid) };
    v.insert(v.end(), payload.begin(), payload.end());
    uint16_t crc = data_crc16(payload);
    v.push_back((uint8_t)(crc & 0xff));
    v.push_back((uint8_t)(crc >> 8));
    return v;
}

// ── DUT driving ──────────────────────────────────────────────────────────

struct RxResult {
    uint8_t shadow[BUF_BYTES];
    int writes = 0;
    bool bad_write_addr = false;
    int done_pulses = 0;
    uint8_t pid = 0;
    bool pid_ok = false;
    int len = -1;
    bool crc_ok = false;
    bool err = false;
    bool ovf = false;
};

// One clock: shadow any buffer write settled before the edge, and pick up
// the registered done pulse plus the latched results after it.
static void cycle(Vusbhc_pkt_rx* dut, RxResult* r) {
    dut->i_clk = 0;
    dut->eval();
    if (dut->o_buf_we) {
        if (dut->o_buf_addr >= BUF_BYTES)
            r->bad_write_addr = true;
        else
            r->shadow[dut->o_buf_addr] = dut->o_buf_data;
        r->writes++;
    }
    dut->i_clk = 1;
    dut->eval();
    if (dut->o_done) {
        r->done_pulses++;
        r->pid = dut->o_pid;
        r->pid_ok = dut->o_pid_ok;
        r->len = dut->o_len;
        r->crc_ok = dut->o_crc_ok;
        r->err = dut->o_err;
        r->ovf = dut->o_overflow;
    }
}

// Feed one packet window: SYNC-like lead-in, bytes strobed every `pace`
// cycles, an optional one-cycle line error after the bytes, then EOP.
static RxResult feed_packet(Vusbhc_pkt_rx* dut,
                            const std::vector<uint8_t>& bytes,
                            int pace, bool inject_err) {
    RxResult r;
    memset(r.shadow, 0, sizeof(r.shadow));

    dut->i_rx_active = 1;
    for (int i = 0; i < 8; i++)
        cycle(dut, &r);

    for (uint8_t b : bytes) {
        dut->i_rx_data = b;
        dut->i_rx_valid = 1;
        cycle(dut, &r);
        dut->i_rx_valid = 0;
        for (int i = 0; i < pace - 1; i++)
            cycle(dut, &r);
    }

    if (inject_err) {
        dut->i_rx_error = 1;
        cycle(dut, &r);
        dut->i_rx_error = 0;
    }

    // The EOP tail before the window closes.
    cycle(dut, &r);
    cycle(dut, &r);
    dut->i_rx_active = 0;
    for (int i = 0; i < 4; i++)
        cycle(dut, &r);
    return r;
}

// ── Checking ─────────────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

// payload == nullptr skips the buffer-content comparison (corrupt streams
// still store bytes; only intact ones are compared).
static void check_rx(const char* name, const RxResult& got,
                     uint8_t pid, bool pid_ok, int len, bool crc_ok,
                     bool err, bool ovf, int writes,
                     const std::vector<uint8_t>* payload) {
    bool ok = got.done_pulses == 1 && !got.bad_write_addr &&
              got.pid == pid && got.pid_ok == pid_ok && got.len == len &&
              got.crc_ok == crc_ok && got.err == err && got.ovf == ovf &&
              got.writes == writes;
    if (ok && payload) {
        for (int i = 0; i < len && ok; i++)
            ok = got.shadow[i] == (*payload)[i];
    }
    if (ok) {
        g_pass++;
        return;
    }
    printf("MISMATCH %s:\n", name);
    printf("  want pid=%x pid_ok=%d len=%d crc_ok=%d err=%d ovf=%d writes=%d\n",
           pid, pid_ok, len, crc_ok, err, ovf, writes);
    printf("  got  pid=%x pid_ok=%d len=%d crc_ok=%d err=%d ovf=%d writes=%d"
           " done=%d bad_addr=%d\n",
           got.pid, got.pid_ok, got.len, got.crc_ok, got.err, got.ovf,
           got.writes, got.done_pulses, got.bad_write_addr);
    g_fail++;
}

int main() {
    Vusbhc_pkt_rx* dut = new Vusbhc_pkt_rx;

    // Power-on reset (held for two edges, matching the testbench convention).
    RxResult scratch;
    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_rx_data = 0;
    dut->i_rx_valid = 0;
    dut->i_rx_active = 0;
    dut->i_rx_error = 0;
    cycle(dut, &scratch);
    cycle(dut, &scratch);
    dut->i_rst = 0;

    const int FS_PACE = 40;

    // Handshakes: a bare, check-nibble-valid PID; no stores, no CRC claim.
    for (auto pid : {PID_ACK, PID_NAK, PID_STALL}) {
        RxResult r = feed_packet(dut, {pid_byte(pid)}, FS_PACE, false);
        check_rx("handshake", r, pid, true, 0, false, false, false, 0, nullptr);
    }

    // A corrupted check nibble must not classify (0x69 -> 0xE9).
    check_rx("bad PID nibble",
             feed_packet(dut, {0xE9}, FS_PACE, false),
             0x9, false, 0, false, false, false, 0, nullptr);

    // A handshake trailing garbage stores nothing.
    check_rx("handshake + trailing byte",
             feed_packet(dut, {pid_byte(PID_ACK), 0x5A}, FS_PACE, false),
             PID_ACK, true, 0, false, false, false, 0, nullptr);

    // Data packets: zero-length, short, and the 64-byte maximum consumed a
    // byte per cycle.
    std::vector<uint8_t> empty;
    check_rx("DATA0 zero-length",
             feed_packet(dut, data_packet(PID_DATA0, empty), FS_PACE, false),
             PID_DATA0, true, 0, true, false, false, 2, &empty);

    std::vector<uint8_t> deadbeef{0xde, 0xad, 0xbe, 0xef};
    check_rx("DATA1 deadbeef",
             feed_packet(dut, data_packet(PID_DATA1, deadbeef), FS_PACE, false),
             PID_DATA1, true, 4, true, false, false, 6, &deadbeef);

    std::vector<uint8_t> ramp;
    for (int i = 0; i < BUF_BYTES; i++)
        ramp.push_back((uint8_t)(i * 7 + 1));
    check_rx("DATA0 64B pace=1",
             feed_packet(dut, data_packet(PID_DATA0, ramp), 1, false),
             PID_DATA0, true, 64, true, false, false, 64, &ramp);

    // Corruption: a payload bit-flip and a CRC bit-flip both break the
    // residual; the length accounting is unaffected.
    {
        std::vector<uint8_t> pkt = data_packet(PID_DATA1, deadbeef);
        pkt[2] ^= 0x10;
        check_rx("corrupt payload byte", feed_packet(dut, pkt, FS_PACE, false),
                 PID_DATA1, true, 4, false, false, false, 6, nullptr);
        pkt = data_packet(PID_DATA1, deadbeef);
        pkt[5] ^= 0x01;
        check_rx("corrupt CRC byte", feed_packet(dut, pkt, FS_PACE, false),
                 PID_DATA1, true, 4, false, false, false, 6, nullptr);
    }

    // Runts: a data PID with fewer bytes than a CRC field can never be
    // CRC-good, whatever the LFSR holds.
    check_rx("runt: bare data PID",
             feed_packet(dut, {pid_byte(PID_DATA0)}, FS_PACE, false),
             PID_DATA0, true, 0, false, false, false, 0, nullptr);
    check_rx("runt: one byte",
             feed_packet(dut, {pid_byte(PID_DATA0), 0x42}, FS_PACE, false),
             PID_DATA0, true, 0, false, false, false, 1, nullptr);

    // One byte past the buffer: stores clamp, the reported length is what
    // was stored, and the overflow flag is the software's signal.
    {
        std::vector<uint8_t> big = ramp;
        big.push_back(0x99);
        check_rx("overflow 65B",
                 feed_packet(dut, data_packet(PID_DATA0, big), 1, false),
                 PID_DATA0, true, 64, true, false, true, 64, &ramp);
    }

    // A PHY line error in the window is latched into the result.
    check_rx("line error flagged",
             feed_packet(dut, data_packet(PID_DATA1, deadbeef), FS_PACE, true),
             PID_DATA1, true, 4, true, true, false, 6, &deadbeef);

    // An empty window (no bytes at all) classifies as nothing.
    check_rx("empty window", feed_packet(dut, {}, FS_PACE, false),
             0, false, 0, false, false, false, 0, nullptr);

    // A good packet right after a bad one: per-packet state must reset.
    check_rx("good after bad",
             feed_packet(dut, data_packet(PID_DATA0, deadbeef), 3, false),
             PID_DATA0, true, 4, true, false, false, 6, &deadbeef);

    // Deterministic pseudo-random spread: length, pace, and an occasional
    // post-PID bit flip (fixed LCG seed, so runs are reproducible).
    uint32_t lcg = 0xBADC0DEu;
    const int paces[3] = {1, 3, 40};
    for (int n = 0; n < 64; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = (int)(lcg >> 26);                  // 0..63
        int pace = paces[(lcg >> 8) % 3];
        bool corrupt = ((lcg >> 16) & 0x7) == 0;     // ~1 in 8
        std::vector<uint8_t> payload;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            payload.push_back((uint8_t)(lcg >> 24));
        }
        uint8_t pid = (n & 1) ? PID_DATA1 : PID_DATA0;
        std::vector<uint8_t> pkt = data_packet(pid, payload);
        if (corrupt) {
            lcg = lcg * 1664525u + 1013904223u;
            size_t pos = 1 + (lcg >> 8) % (pkt.size() - 1);   // post-PID
            pkt[pos] ^= (uint8_t)(1u << ((lcg >> 24) & 0x7));
        }
        char name[64];
        snprintf(name, sizeof(name), "random len=%d pace=%d corrupt=%d",
                 len, pace, corrupt);
        int writes = len + 2 > BUF_BYTES ? BUF_BYTES : len + 2;
        check_rx(name, feed_packet(dut, pkt, pace, false),
                 pid, true, len, !corrupt, false, false, writes,
                 corrupt ? nullptr : &payload);
    }

    printf("usbhc_pkt_rx: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}
