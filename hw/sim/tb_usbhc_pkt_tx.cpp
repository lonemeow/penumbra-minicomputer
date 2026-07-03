// Verilator testbench for the Penumbra USB host packet transmitter
//
// Plays the PHY side of the UTMI-shaped seam (consumes bytes at a
// programmable pace after a SYNC-like delay) and the data buffer's
// synchronous read port, then checks whole packets — PID byte, field or
// payload, on-wire CRC — against a C++ reference mirroring
// sw/tools/usb_crc.py. Literal golden values printed by that script anchor
// the C++ reference itself before any RTL runs.
//
// Pacing matters here: tokens are consumed at line-realistic gaps (the
// serial CRC5 needs its 11-cycle head start, which SYNC provides), while
// one data-packet case consumes a byte every cycle to prove the payload
// path sustains the seam's high-speed forward-compatibility rate.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusbhc_pkt_tx.h"

// PID nibbles (usb_pkg::usb_pid_e).
static const uint8_t PID_OUT   = 0x1;
static const uint8_t PID_IN    = 0x9;
static const uint8_t PID_SOF   = 0x5;
static const uint8_t PID_SETUP = 0xD;
static const uint8_t PID_DATA0 = 0x3;
static const uint8_t PID_DATA1 = 0xB;
static const uint8_t PID_ACK   = 0x2;

static uint8_t pid_byte(uint8_t pid) {
    return (uint8_t)(((~pid & 0xf) << 4) | (pid & 0xf));
}

// ── C++ reference, step-for-step usb_crc.py ──────────────────────────────

static uint32_t reflect(uint32_t value, int width) {
    uint32_t result = 0;
    for (int i = 0; i < width; i++)
        if (value & (1u << i))
            result |= 1u << (width - 1 - i);
    return result;
}

// Left-shift LFSR remainder over the 11-bit token field, fed LSB first.
static uint8_t crc5_remainder(uint16_t field) {
    uint8_t crc = 0x1f;
    for (int i = 0; i < 11; i++) {
        int feedback = ((crc >> 4) & 1) ^ ((field >> i) & 1);
        crc = (uint8_t)((crc << 1) & 0x1f);
        if (feedback)
            crc ^= 0x05;
    }
    return crc;
}

// Full on-wire token CRC5: remainder, then invert and bit-reverse.
static uint8_t token_crc5(uint16_t field) {
    return (uint8_t)reflect(crc5_remainder(field) ^ 0x1f, 5);
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

// Full on-wire data CRC16: remainder, then invert and bit-reverse.
static uint16_t data_crc16(const std::vector<uint8_t>& data) {
    return (uint16_t)reflect(crc16_remainder(data) ^ 0xffff, 16);
}

// ── Expected packet images ───────────────────────────────────────────────

// The 11-bit token field: ADDR in the low seven bits, ENDP above — the
// LSB-first wire order puts ADDR first.
static uint16_t token_field(uint8_t addr, uint8_t endp) {
    return (uint16_t)(((endp & 0xf) << 7) | (addr & 0x7f));
}

static std::vector<uint8_t> expect_token(uint8_t pid, uint16_t field) {
    uint8_t crc = token_crc5(field);
    return { pid_byte(pid),
             (uint8_t)(field & 0xff),
             (uint8_t)((crc << 3) | ((field >> 8) & 0x7)) };
}

static std::vector<uint8_t> expect_data(uint8_t pid,
                                        const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> v{ pid_byte(pid) };
    v.insert(v.end(), payload.begin(), payload.end());
    uint16_t crc = data_crc16(payload);
    v.push_back((uint8_t)(crc & 0xff));   // low byte leads on the wire
    v.push_back((uint8_t)(crc >> 8));
    return v;
}

// ── DUT driving ──────────────────────────────────────────────────────────

static uint8_t g_buf[128];   // the data buffer behind the payload read port

// One clock. The payload port models a BRAM synchronous read: the address
// standing at the rising edge yields its byte for the following cycle. The
// address must be captured before the edge — afterwards the DUT's
// combinational next-index has already moved on under the cycle's inputs.
static void tick(Vusbhc_pkt_tx* dut) {
    dut->i_clk = 0;
    dut->eval();
    uint8_t addr = (uint8_t)(dut->o_pl_addr & 0x7f);
    dut->i_clk = 1;
    dut->eval();
    dut->i_pl_data = g_buf[addr];
}

struct Result {
    std::vector<uint8_t> bytes;
    bool done_pulse = false;
    bool timeout = false;
};

// Issue one request and consume the packet: the first byte after sync_delay
// cycles (the PHY's SYNC time), each later byte `pace` cycles after the
// previous consume. pace 1 = a byte every cycle.
static Result send_packet(Vusbhc_pkt_tx* dut, uint8_t pid, uint16_t field,
                          int len, bool keepalive, int sync_delay, int pace) {
    Result r;

    dut->i_start = 1;
    dut->i_pid = pid;
    dut->i_field = field;
    dut->i_len = (uint8_t)len;
    dut->i_keepalive = keepalive;
    tick(dut);
    dut->i_start = 0;

    int countdown = sync_delay;
    for (int cycles = 0; dut->o_busy; cycles++) {
        if (cycles > 20000) {
            r.timeout = true;
            break;
        }
        countdown--;
        dut->i_tx_ready = (countdown <= 0);
        dut->i_clk = 0;
        dut->eval();
        if (dut->o_tx_valid && dut->i_tx_ready) {
            r.bytes.push_back(dut->o_tx_data);
            countdown = pace;
        }
        if (dut->o_done)
            r.done_pulse = true;
        uint8_t addr = (uint8_t)(dut->o_pl_addr & 0x7f);
        dut->i_clk = 1;
        dut->eval();
        dut->i_pl_data = g_buf[addr];
    }
    dut->i_tx_ready = 0;
    return r;
}

// ── Checking ─────────────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void dump(const char* tag, const std::vector<uint8_t>& v) {
    printf("  %s:", tag);
    for (uint8_t b : v)
        printf(" %02x", b);
    printf("\n");
}

static void check(const char* name, const Result& got,
                  const std::vector<uint8_t>& want) {
    if (!got.timeout && got.done_pulse && got.bytes == want) {
        g_pass++;
        return;
    }
    printf("MISMATCH %s%s%s:\n", name,
           got.timeout ? " (timeout)" : "",
           got.done_pulse ? "" : " (no done pulse)");
    dump("want", want);
    dump("got ", got.bytes);
    g_fail++;
}

// The C++ reference must reproduce the table sw/tools/usb_crc.py prints —
// a wrong reference would wave a wrong implementation through.
static bool check_reference_anchors() {
    struct { uint8_t addr, endp, crc; } t5[] = {
        {0x00, 0x0, 0x02}, {0x15, 0xE, 0x1D}, {0x3A, 0xA, 0x07},
        {0x7F, 0xF, 0x08},
    };
    for (auto& t : t5) {
        if (token_crc5(token_field(t.addr, t.endp)) != t.crc) {
            printf("reference anchor: token_crc5(%02x,%x) != %02x\n",
                   t.addr, t.endp, t.crc);
            return false;
        }
    }

    std::vector<uint8_t> deadbeef{0xde, 0xad, 0xbe, 0xef};
    std::vector<uint8_t> ramp;
    for (int i = 0; i < 64; i++)
        ramp.push_back((uint8_t)i);
    if (data_crc16({}) != 0x0000 || data_crc16({0x00}) != 0xbf40 ||
        data_crc16(deadbeef) != 0x3e64 || data_crc16(ramp) != 0xf726) {
        printf("reference anchor: data_crc16 table mismatch\n");
        return false;
    }

    if (pid_byte(PID_SOF) != 0xa5 || pid_byte(PID_ACK) != 0xd2 ||
        pid_byte(PID_DATA0) != 0xc3 || pid_byte(PID_SETUP) != 0x2d) {
        printf("reference anchor: PID byte forming mismatch\n");
        return false;
    }
    return true;
}

int main() {
    if (!check_reference_anchors()) {
        printf("usbhc_pkt_tx: reference model does not match usb_crc.py\n");
        return 1;
    }

    Vusbhc_pkt_tx* dut = new Vusbhc_pkt_tx;

    // Power-on reset (held for two edges, matching the testbench convention).
    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_start = 0;
    dut->i_pid = 0;
    dut->i_field = 0;
    dut->i_len = 0;
    dut->i_keepalive = 0;
    dut->i_tx_ready = 0;
    dut->i_pl_data = 0;
    tick(dut);
    tick(dut);
    dut->i_rst = 0;

    // Full-speed-realistic pacing: 8 SYNC bit times, 8 bit times per byte,
    // at 5 clocks per bit.
    const int FS_SYNC = 40, FS_PACE = 40;

    // Tokens, covering the field corners and both realistic and tight-but-
    // legal pacing (the CRC5 assertion polices the tight case).
    struct { const char* name; uint8_t pid, addr, endp; int sync, pace; } toks[] = {
        {"SETUP a=15 e=E", PID_SETUP, 0x15, 0xE, FS_SYNC, FS_PACE},
        {"IN a=00 e=0",    PID_IN,    0x00, 0x0, FS_SYNC, FS_PACE},
        {"OUT a=7F e=F",   PID_OUT,   0x7F, 0xF, 8, 8},
        {"IN a=3A e=A",    PID_IN,    0x3A, 0xA, 12, 12},
    };
    for (auto& t : toks) {
        uint16_t field = token_field(t.addr, t.endp);
        check(t.name, send_packet(dut, t.pid, field, 0, false, t.sync, t.pace),
              expect_token(t.pid, field));
    }

    // SOF carries the frame number in the same token shape.
    check("SOF frame=2C9",
          send_packet(dut, PID_SOF, 0x2C9, 0, false, FS_SYNC, FS_PACE),
          expect_token(PID_SOF, 0x2C9));

    // Data packets: zero-length (status stage), a short payload, and the
    // 64-byte full-speed max consumed one byte per cycle — the payload path
    // must sustain the seam's forward-compatible byte rate.
    check("DATA0 zero-length",
          send_packet(dut, PID_DATA0, 0, 0, false, FS_SYNC, FS_PACE),
          expect_data(PID_DATA0, {}));

    std::vector<uint8_t> deadbeef{0xde, 0xad, 0xbe, 0xef};
    for (size_t i = 0; i < deadbeef.size(); i++)
        g_buf[i] = deadbeef[i];
    check("DATA1 deadbeef",
          send_packet(dut, PID_DATA1, 0, 4, false, FS_SYNC, FS_PACE),
          expect_data(PID_DATA1, deadbeef));
    // Same packet again: the CRC must re-seed between packets.
    check("DATA1 deadbeef again",
          send_packet(dut, PID_DATA1, 0, 4, false, FS_SYNC, 3),
          expect_data(PID_DATA1, deadbeef));

    std::vector<uint8_t> ramp;
    for (int i = 0; i < 64; i++)
        ramp.push_back((uint8_t)i);
    for (size_t i = 0; i < ramp.size(); i++)
        g_buf[i] = ramp[i];
    check("DATA0 64B pace=1",
          send_packet(dut, PID_DATA0, 0, 64, false, 8, 1),
          expect_data(PID_DATA0, ramp));

    // Handshake and keep-alive are bare PID bytes.
    check("ACK", send_packet(dut, PID_ACK, 0, 0, false, FS_SYNC, FS_PACE),
          {pid_byte(PID_ACK)});
    check("LS keep-alive",
          send_packet(dut, PID_SOF, 0, 0, true, FS_SYNC, FS_PACE),
          {pid_byte(PID_SOF)});

    // A token straight after a data packet (fresh CRC5 over a fresh field).
    check("SETUP after data",
          send_packet(dut, PID_SETUP, token_field(0x01, 0x0), 0, false,
                      FS_SYNC, FS_PACE),
          expect_token(PID_SETUP, token_field(0x01, 0x0)));

    // Deterministic pseudo-random spread: payload length and pace vary,
    // PIDs alternate (fixed LCG seed, so runs are reproducible).
    uint32_t lcg = 0xC0FFEEu;
    const int paces[3] = {1, 3, 40};
    for (int n = 0; n < 64; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = (int)(lcg >> 26);                  // 0..63
        int pace = paces[(lcg >> 8) % 3];
        std::vector<uint8_t> payload;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            payload.push_back((uint8_t)(lcg >> 24));
            g_buf[i] = payload[i];
        }
        uint8_t pid = (n & 1) ? PID_DATA1 : PID_DATA0;
        char name[48];
        snprintf(name, sizeof(name), "random len=%d pace=%d", len, pace);
        check(name, send_packet(dut, pid, 0, len, false, 8, pace),
              expect_data(pid, payload));
    }

    printf("usbhc_pkt_tx: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}
