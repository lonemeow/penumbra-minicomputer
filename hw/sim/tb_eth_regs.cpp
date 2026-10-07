// Verilator testbench for the Ethernet adapter's register block
//
// A bus master drives the block by the bus's rules, with a model of each
// thing behind it: the two slot memories, the PHY management master, and
// the counts and the dropped-frame pulse that come back from the two
// PHY-side clocks. The checks are the programmer's reference
// (doc/system/devices/ethernet.md) register by register: reset values,
// what can be written and what reads back, reserved bits reading zero,
// the two interrupt conditions across the counts' wrap, a management
// transaction from write to answer, and the apertures, where a write to
// the transmit ring commits exactly the byte lanes it enables and the
// receive ring reads back what the receiver stored.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include "Veth_regs.h"

namespace {

Veth_regs* dut;
int pass = 0, fail = 0;
long now = 0;                       // clock cycles since the start

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 30) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    if (fail >= 500) {
        printf("eth_regs: gave up after %d failures\n", fail);
        exit(1);
    }
}

// The module's defaults.
constexpr int RX_SLOTS   = 32;
constexpr int TX_SLOTS   = 8;
constexpr int SLOT_BYTES = 2048;

// The register map.
enum : uint32_t {
    CAP = 0x00, IRQ_STATUS = 0x04, IRQ_ENABLE = 0x08, CTRL = 0x0c, LINK = 0x10,
    MDIO = 0x14, ADDR_LO = 0x18, ADDR_HI = 0x1c, FACTORY_LO = 0x20, FACTORY_HI = 0x24,
    RX_GEOM = 0x40, RX_PROD = 0x44, RX_CONS = 0x48, RX_DROP = 0x4c,
    TX_GEOM = 0x50, TX_PROD = 0x54, TX_CONS = 0x58, TX_WM = 0x5c,
    TX_APERTURE = 0x8000, RX_APERTURE = 0x10000,
};
constexpr uint32_t IRQ_RX = 1, IRQ_TX = 2;
constexpr uint32_t MDIO_BUSY = 1u << 31, MDIO_WRITE = 1u << 30;

// Where autoconfig put the device: the block decodes the offset alone.
constexpr uint32_t BASE = 0xff0a0000u;

// ── Behind the block ─────────────────────────────────────────
uint32_t rx_mem[RX_SLOTS * SLOT_BYTES / 4];
uint32_t tx_mem[TX_SLOTS * SLOT_BYTES / 4];
long     tx_write_cycles = 0;       // cycles a transmit write enable was up

struct {
    int      starts = 0;
    bool     write = false;
    int      phy_addr = 0, reg_addr = 0;
    uint16_t wdata = 0;
    int      busy_left = 0;         // cycles the transaction still runs
    uint16_t answer = 0;            // what a read will return
} mdio;

void tick() {
    dut->i_clk = 0;
    dut->eval();

    // What the edge will act on.
    uint32_t rx_addr = dut->o_rx_mem_addr;
    uint32_t tx_addr = dut->o_tx_mem_addr;
    int      tx_we   = dut->o_tx_mem_we;
    uint32_t tx_data = dut->o_tx_mem_wdata;
    bool     start   = dut->o_mdio_start;
    if (start) {
        mdio.starts++;
        mdio.write    = dut->o_mdio_write;
        mdio.phy_addr = dut->o_mdio_phy_addr;
        mdio.reg_addr = dut->o_mdio_reg_addr;
        mdio.wdata    = dut->o_mdio_wdata;
    }

    dut->i_clk = 1;
    dut->eval();
    now++;

    dut->i_rx_mem_rdata = rx_mem[rx_addr];
    if (tx_we) {
        tx_write_cycles++;
        for (int lane = 0; lane < 4; lane++)
            if (tx_we & (1 << lane))
                tx_mem[tx_addr] = (tx_mem[tx_addr] & ~(0xffu << (8 * lane))) |
                                  (tx_data & (0xffu << (8 * lane)));
    }

    // The management master: busy from the cycle after a start, for a
    // while, then one cycle of done with a read's answer.
    dut->i_mdio_done = 0;
    if (start) {
        mdio.busy_left = 40;
    } else if (mdio.busy_left > 0 && --mdio.busy_left == 0) {
        dut->i_mdio_done = 1;
        dut->i_mdio_rdata = mdio.answer;
    }
    dut->i_mdio_busy = mdio.busy_left > 0;

    // What the models now present reaches the outputs that pass it on.
    dut->eval();
}

// One access by the bus's rules: the strobe goes up with the address, the
// device answers in the next cycle, and the strobe is held through it.
// With `hold`, the strobe is left up afterwards for the caller to begin
// the next access on.
uint32_t access(bool write, uint32_t offset, uint32_t data, int byte_en = 0xf,
                bool hold = false) {
    dut->i_addr    = BASE + offset;
    dut->i_wdata   = data;
    dut->i_byte_en = byte_en;
    dut->i_we      = write;
    dut->i_re      = !write;
    dut->eval();
    check(!dut->o_ack, "%s of %05x: an ack before the access", write ? "write" : "read", offset);
    tick();
    check(dut->o_ack, "%s of %05x: no ack the cycle after", write ? "write" : "read", offset);
    uint32_t rdata = dut->o_rdata;
    tick();
    check(!dut->o_ack, "%s of %05x: the ack lasted two cycles", write ? "write" : "read", offset);
    if (!hold) {
        dut->i_we = 0;
        dut->i_re = 0;
    }
    return rdata;
}

uint32_t rd(uint32_t offset) { return access(false, offset, 0xdeadbeefu); }
void     wr(uint32_t offset, uint32_t data) { access(true, offset, data); }

void expect(uint32_t offset, uint32_t want, const char* what) {
    uint32_t got = rd(offset);
    check(got == want, "%s: register %02x reads %08x, want %08x", what, offset, got, want);
}

void reset(int speeds) {
    dut->i_speeds = speeds;
    dut->i_rst = 1;
    dut->i_we = dut->i_re = 0;
    tick();
    tick();
    dut->i_rst = 0;
    mdio.busy_left = 0;
    dut->i_mdio_busy = 0;
}

uint32_t geom(int slots) { return ((uint32_t)SLOT_BYTES << 16) | (uint32_t)slots; }

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_regs;
    dut->i_clk = 0;
    dut->i_addr = 0;
    dut->i_wdata = 0;
    dut->i_byte_en = 0;
    dut->i_factory_addr = 0;
    dut->i_factory_valid = 0;
    dut->i_rx_prod = 0;
    dut->i_rx_drop = 0;
    dut->i_tx_cons = 0;
    dut->i_mdio_busy = 0;
    dut->i_mdio_done = 0;
    dut->i_mdio_rdata = 0;
    dut->i_rx_mem_rdata = 0;

    // ── Reset values, for each set of speeds a PHY adapter may carry ──
    // CTRL.SPEED resets to a speed the device carries: the slowest.
    const int speed_sets[][2] = {{0b011, 0}, {0b111, 0}, {0b110, 1}, {0b100, 2}, {0b001, 0}};
    for (const auto& s : speed_sets) {
        reset(s[0]);
        expect(CAP, 0x00000301u | ((uint32_t)s[0] << 16), "CAP: version 1, MDIO, ADDR, speeds");
        expect(IRQ_STATUS, 0, "IRQ_STATUS at reset");
        expect(IRQ_ENABLE, 0, "IRQ_ENABLE at reset");
        expect(CTRL, (uint32_t)s[1] << 8, "CTRL at reset: nothing enabled, the slowest speed");
        expect(LINK, 0, "LINK on a device with a managed PHY");
        expect(MDIO, 0, "MDIO at reset");
        expect(ADDR_LO, 0, "ADDR_LO at reset");
        expect(ADDR_HI, 0, "ADDR_HI at reset");
        expect(RX_GEOM, geom(RX_SLOTS), "RX_GEOM");
        expect(RX_PROD, 0, "RX_PROD at reset");
        expect(RX_CONS, 0, "RX_CONS at reset");
        expect(RX_DROP, 0, "RX_DROP at reset");
        expect(TX_GEOM, geom(TX_SLOTS), "TX_GEOM");
        expect(TX_PROD, 0, "TX_PROD at reset");
        expect(TX_CONS, 0, "TX_CONS at reset");
        expect(TX_WM, 0, "TX_WM at reset");
        check(!dut->o_irq, "interrupt at reset");
        check(!dut->o_rx_en && !dut->o_tx_en, "an enable set at reset");
    }
    reset(0b011);

    // ── What can be written, and what reads back ──
    struct { uint32_t offset, mask; const char* name; } writable[] = {
        {IRQ_ENABLE, 0x00000003u, "IRQ_ENABLE"}, {CTRL, 0x0000030fu, "CTRL"},
        {ADDR_LO, 0xffffffffu, "ADDR_LO"},       {ADDR_HI, 0x0000ffffu, "ADDR_HI"},
        {RX_CONS, 0x0000ffffu, "RX_CONS"},       {TX_PROD, 0x0000ffffu, "TX_PROD"},
        {TX_WM, 0x0000ffffu, "TX_WM"},
    };
    // With every speed carried, every SPEED code but 3 is one to write.
    reset(0b111);
    for (const auto& r : writable) {
        for (uint32_t v : {0xffffffffu, 0xa5a5a5a5u, 0x5a5a5a5au, 0x00000000u}) {
            if (r.offset == CTRL && (v & 0x300u) == 0x300u) v &= ~0x100u;
            wr(r.offset, v);
            uint32_t got = rd(r.offset);
            check(got == (v & r.mask), "%s: wrote %08x, reads %08x, want %08x", r.name, v, got,
                  v & r.mask);
        }
    }

    // What they drive: the values the crossings carry.
    wr(CTRL, 0x0000010du);          // RX_EN, PROMISC, ALLMULTI, speed 1
    check(dut->o_rx_en && !dut->o_tx_en && dut->o_promisc && dut->o_allmulti &&
          dut->o_speed == 1, "CTRL 010d: rx_en %d tx_en %d promisc %d allmulti %d speed %d",
          dut->o_rx_en, dut->o_tx_en, dut->o_promisc, dut->o_allmulti, dut->o_speed);
    wr(CTRL, 0x00000002u);          // TX_EN alone
    check(!dut->o_rx_en && dut->o_tx_en && !dut->o_promisc && !dut->o_allmulti &&
          dut->o_speed == 0, "CTRL 0002: rx_en %d tx_en %d promisc %d allmulti %d speed %d",
          dut->o_rx_en, dut->o_tx_en, dut->o_promisc, dut->o_allmulti, dut->o_speed);
    wr(ADDR_LO, 0x33221102u);
    wr(ADDR_HI, 0x00005544u);
    check(dut->o_addr == 0x554433221102ULL, "station address out: %012llx",
          (unsigned long long)dut->o_addr);
    wr(RX_CONS, 0x1234);
    wr(TX_PROD, 0xabcd);
    check(dut->o_rx_cons == 0x1234 && dut->o_tx_prod == 0xabcd, "counts out: %04x %04x",
          dut->o_rx_cons, dut->o_tx_prod);
    reset(0b011);

    // ── What cannot be written ──
    dut->i_factory_addr = 0x0a0b0c0d0e0fULL;
    dut->i_factory_valid = 1;
    dut->i_rx_prod = 0x0102;
    dut->i_tx_cons = 0x0304;
    struct { uint32_t offset; const char* name; } read_only[] = {
        {CAP, "CAP"}, {IRQ_STATUS, "IRQ_STATUS"}, {LINK, "LINK"}, {FACTORY_LO, "FACTORY_LO"},
        {FACTORY_HI, "FACTORY_HI"}, {RX_GEOM, "RX_GEOM"}, {RX_PROD, "RX_PROD"},
        {RX_DROP, "RX_DROP"}, {TX_GEOM, "TX_GEOM"}, {TX_CONS, "TX_CONS"},
    };
    for (const auto& r : read_only) {
        uint32_t before = rd(r.offset);
        wr(r.offset, ~before);
        uint32_t after = rd(r.offset);
        check(after == before, "%s is read-only: %08x became %08x", r.name, before, after);
    }
    expect(RX_PROD, 0x0102, "RX_PROD shows the receiver's count");
    expect(TX_CONS, 0x0304, "TX_CONS shows the transmitter's count");

    // The factory address, laid out as ADDR, and its VALID bit.
    expect(FACTORY_LO, 0x0c0d0e0fu, "FACTORY_LO");
    expect(FACTORY_HI, 0x80000a0bu, "FACTORY_HI with the address held");
    dut->i_factory_valid = 0;
    expect(FACTORY_HI, 0x00000a0bu, "FACTORY_HI before the address is held");

    // ── The interrupt conditions ──
    // RX: the receive ring holds frames not released, RX_PROD != RX_CONS.
    // TX: fewer frames are queued than TX_WM, TX_PROD - TX_CONS < TX_WM.
    // Both hold across the counts' wrap, and each bit shows its condition
    // whether or not it is enabled. A ring holds from none to SLOTS frames,
    // so those are the states to try.
    const uint16_t counts[] = {0, 1, 2, 7, 8, 0x7fff, 0x8000, 0xfffe, 0xffff};
    for (uint16_t cons : counts) {
        for (int held = 0; held <= RX_SLOTS; held++) {
            dut->i_rx_prod = (uint16_t)(cons + held);
            wr(RX_CONS, cons);
            uint32_t status = rd(IRQ_STATUS);
            check(((status & IRQ_RX) != 0) == (held != 0),
                  "IRQ_STATUS.RX is %d with %d frames held (RX_PROD %04x, RX_CONS %04x)",
                  (status & IRQ_RX) != 0, held, (uint16_t)(cons + held), cons);
        }
    }
    dut->i_rx_prod = 0;
    wr(RX_CONS, 0);

    const uint16_t marks[] = {0, 1, 2, 8, 9, 0x7fff, 0xffff};
    for (uint16_t cons : counts) {
        for (int queued = 0; queued <= TX_SLOTS; queued++) {
            for (uint16_t wm : marks) {
                dut->i_tx_cons = cons;
                wr(TX_PROD, (uint16_t)(cons + queued));
                wr(TX_WM, wm);
                uint32_t status = rd(IRQ_STATUS);
                check(((status & IRQ_TX) != 0) == (queued < wm),
                      "IRQ_STATUS.TX is %d with %d queued (TX_PROD %04x, TX_CONS %04x), TX_WM %u",
                      (status & IRQ_TX) != 0, queued, (uint16_t)(cons + queued), cons, wm);
            }
        }
    }
    dut->i_tx_cons = 0;
    wr(TX_PROD, 0);
    wr(TX_WM, 0);

    // The output is the enabled conditions, and only those.
    for (int rx = 0; rx <= 1; rx++) {
        for (int tx = 0; tx <= 1; tx++) {
            dut->i_rx_prod = rx;            // one frame unreleased, or none
            wr(TX_WM, tx);                  // nothing queued: below 1, not below 0
            for (uint32_t enable = 0; enable <= 3; enable++) {
                wr(IRQ_ENABLE, enable);
                uint32_t status = (rx ? IRQ_RX : 0) | (tx ? IRQ_TX : 0);
                expect(IRQ_STATUS, status, "IRQ_STATUS, whatever is enabled");
                check((dut->o_irq != 0) == ((status & enable) != 0),
                      "interrupt is %d with IRQ_STATUS %x, IRQ_ENABLE %x", dut->o_irq, status,
                      enable);
            }
        }
    }
    dut->i_rx_prod = 0;
    wr(TX_WM, 0);
    wr(IRQ_ENABLE, 0);

    // ── Frames dropped ──
    for (int i = 0; i < 5; i++) {
        dut->i_rx_drop = 1;
        tick();
        dut->i_rx_drop = 0;
        tick();
        tick();
    }
    expect(RX_DROP, 5, "RX_DROP after five frames lost");
    // One arriving in the middle of a read is counted all the same.
    dut->i_rx_drop = 1;
    uint32_t during = rd(RX_DROP);
    dut->i_rx_drop = 0;
    check(during == 5 || during == 6, "RX_DROP read as a frame is lost: %u", during);
    expect(RX_DROP, 7, "RX_DROP after two more cycles of the pulse");

    // ── A management transaction ──
    // A write of MDIO with BUSY clear starts one, with the fields written.
    {
        int starts = mdio.starts;
        mdio.answer = 0x796d;
        wr(MDIO, (1u << 24) | (2u << 16));              // read PHY 1, register 2
        check(mdio.starts == starts + 1 && !mdio.write && mdio.phy_addr == 1 &&
              mdio.reg_addr == 2,
              "MDIO read: %d starts, write %d, PHY %d, register %d", mdio.starts - starts,
              mdio.write, mdio.phy_addr, mdio.reg_addr);
        expect(MDIO, MDIO_BUSY | (1u << 24) | (2u << 16), "MDIO while the read runs");

        // A write while BUSY is set is ignored.
        wr(MDIO, MDIO_WRITE | (9u << 24) | (9u << 16) | 0xdead);
        check(mdio.starts == starts + 1, "a write of MDIO while BUSY started a transaction");
        expect(MDIO, MDIO_BUSY | (1u << 24) | (2u << 16), "MDIO after a write while BUSY");

        for (int i = 0; i < 60; i++) tick();
        expect(MDIO, (1u << 24) | (2u << 16) | 0x796d, "MDIO after the read: DATA is the answer");

        // A write transaction: the data goes out, and DATA stays as written.
        mdio.answer = 0xffff;
        wr(MDIO, MDIO_WRITE | (1u << 24) | (4u << 16) | 0x01e1);
        check(mdio.starts == starts + 2 && mdio.write && mdio.phy_addr == 1 &&
              mdio.reg_addr == 4 && mdio.wdata == 0x01e1,
              "MDIO write: %d starts, write %d, PHY %d, register %d, data %04x",
              mdio.starts - starts, mdio.write, mdio.phy_addr, mdio.reg_addr, mdio.wdata);
        for (int i = 0; i < 60; i++) tick();
        expect(MDIO, MDIO_WRITE | (1u << 24) | (4u << 16) | 0x01e1,
               "MDIO after the write: the fields as written");

        // Reserved bits written as ones read zero, and every field crosses.
        wr(MDIO, 0xffffffffu);
        check(mdio.phy_addr == 31 && mdio.reg_addr == 31 && mdio.wdata == 0xffff && mdio.write,
              "MDIO all ones: PHY %d, register %d, data %04x", mdio.phy_addr, mdio.reg_addr,
              mdio.wdata);
        for (int i = 0; i < 60; i++) tick();
        expect(MDIO, 0x5f1fffffu, "MDIO written all ones");
    }

    // ── The transmit aperture ──
    // A write commits exactly the lanes it enables, once, at the word the
    // offset names, and reads of the aperture return zero.
    {
        for (auto& w : tx_mem) w = 0;
        long cycles = tx_write_cycles;
        access(true, TX_APERTURE, 0x44332211u, 0xf);
        check(tx_mem[0] == 0x44332211u, "word write to the ring's first word: %08x", tx_mem[0]);
        check(tx_write_cycles == cycles + 1, "a write enabled the memory for %ld cycles",
              tx_write_cycles - cycles);

        tx_mem[5] = 0xaaaaaaaau;
        const struct { int byte_en; uint32_t want; } lanes[] = {
            {0x1, 0xaaaaaa11u}, {0x2, 0xaaaa22aau}, {0x4, 0xaa33aaaau}, {0x8, 0x44aaaaaau},
            {0x3, 0xaaaa2211u}, {0xc, 0x4433aaaau}, {0x0, 0xaaaaaaaau},
        };
        for (const auto& l : lanes) {
            tx_mem[5] = 0xaaaaaaaau;
            access(true, TX_APERTURE + 5 * 4, 0x44332211u, l.byte_en);
            check(tx_mem[5] == l.want, "write with byte lanes %x: word is %08x, want %08x",
                  l.byte_en, tx_mem[5], l.want);
        }

        // Every slot, at its own place.
        for (int slot = 0; slot < TX_SLOTS; slot++) {
            access(true, TX_APERTURE + slot * SLOT_BYTES + 8, 0x7100u + slot);
            check(tx_mem[slot * SLOT_BYTES / 4 + 2] == 0x7100u + slot,
                  "write to slot %d: %08x", slot, tx_mem[slot * SLOT_BYTES / 4 + 2]);
        }

        tx_mem[0] = 0x12345678u;
        uint32_t got = rd(TX_APERTURE);
        check(got == 0, "the transmit aperture reads %08x, want zero", got);
    }

    // ── The receive aperture ──
    // Reads return what the receiver stored, word for word, and a write
    // changes nothing anywhere.
    {
        for (int i = 0; i < RX_SLOTS * SLOT_BYTES / 4; i++)
            rx_mem[i] = 0xc0000000u | (uint32_t)i;
        for (int slot = 0; slot < RX_SLOTS; slot++) {
            for (int word : {0, 1, 255, SLOT_BYTES / 4 - 1}) {
                uint32_t offset = RX_APERTURE + slot * SLOT_BYTES + word * 4;
                uint32_t got = rd(offset);
                uint32_t want = 0xc0000000u | (uint32_t)(slot * SLOT_BYTES / 4 + word);
                check(got == want, "read of slot %d, word %d: %08x, want %08x", slot, word, got,
                      want);
            }
        }
        long cycles = tx_write_cycles;
        wr(RX_APERTURE + 16, 0xffffffffu);
        check(tx_write_cycles == cycles, "a write to the receive aperture reached a memory");
        expect(CTRL, 0, "CTRL after a write to the receive aperture");
    }

    // ── The strobe held from one access into the next ──
    // A master may keep it up and change the address after the ack: each
    // is its own access and commits once.
    {
        long cycles = tx_write_cycles;
        access(true, TX_APERTURE + 0x40, 0x000000a1u, 0xf, true);
        access(true, TX_APERTURE + 0x44, 0x000000a2u, 0xf, true);
        access(true, TX_WM, 5, 0xf, false);
        check(tx_mem[0x10] == 0xa1 && tx_mem[0x11] == 0xa2,
              "two writes on a held strobe: %08x %08x", tx_mem[0x10], tx_mem[0x11]);
        check(tx_write_cycles == cycles + 2, "two writes enabled the memory for %ld cycles",
              tx_write_cycles - cycles);
        expect(TX_WM, 5, "a register write on a held strobe");
        wr(TX_WM, 0);
    }

    printf("eth_regs: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}
