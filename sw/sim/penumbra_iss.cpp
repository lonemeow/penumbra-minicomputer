// Penumbra Instruction Set Simulator (ISS)
//
// Fast instruction-level simulator for software development.
// No Verilator dependency — compiles with just g++ -O2.
//
// Matches the RTL machine_sim memory map and device set:
//   0x00000000 – 0x00FFFFFF  RAM (16 MB)
//   0xFE000000 – 0xFE00001F  Autoconfig space (when cfg_en)
//   0xFF000000 – 0xFF000FFF  UART (16450-compatible)
//   0xFFFF0000 – 0xFFFFFFFF  Boot ROM (64 KB)
//   SPI at dynamic base (assigned via autoconfig)
//
// Trace output format (compatible with RTL tb_interactive):
//   PC=XXXXXXXX SR=XXXXXXXX [SVNZCV] R1=... R2=... ... R14=...
//
// Usage: penumbra-iss [program.hex] [+sdcard=path] [+trace=path] [+raw]
//                     [+trap-pc0] [+halt-on-break]
//
// +raw enables full raw TTY mode: all control characters (Ctrl-C,
// Ctrl-Z, etc.) pass through to the simulated UART for job control
// inside the guest OS.  Use Ctrl-A as escape prefix:
//   Ctrl-A X     — exit simulator
//   Ctrl-A C     — dump CPU state
//   Ctrl-A B     — send serial BREAK (triggers DDB if enabled)
//   Ctrl-A H     — help
//   Ctrl-A Ctrl-A — send literal Ctrl-A

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <vector>

// ═══════════════════════════════════════════════════════════════
// SD Card Emulator (inline — same protocol as hw/sim/sd_card_sim.h
// but self-contained to avoid build dependency on hw/)
// ═══════════════════════════════════════════════════════════════

class SdCardSim {
public:
    explicit SdCardSim(const char* path) {
        if (!path || !path[0]) return;
        img_ = fopen(path, "r+b");
        if (!img_) img_ = fopen(path, "rb");
        if (!img_) { fprintf(stderr, "[SD] cannot open '%s'\n", path); return; }
        fseek(img_, 0, SEEK_END);
        total_sectors_ = (uint32_t)(ftell(img_) / 512);
        fseek(img_, 0, SEEK_SET);
        fprintf(stderr, "[SD] %u sectors from '%s'\n", total_sectors_, path);
    }
    ~SdCardSim() { if (img_) fclose(img_); }
    bool is_present() const { return img_ != nullptr; }

    void select(bool sel) {
        if (!sel && selected_) {
            cmd_pos_ = 0; state_ = S_IDLE;
            resp_.clear(); resp_idx_ = 0; write_pos_ = 0;
        }
        selected_ = sel;
    }

    uint8_t exchange(uint8_t mosi) {
        if (!selected_ || !img_) return 0xFF;
        // If we're waiting for a data token but a command-start byte
        // arrives instead, the host driver has aborted a write
        // mid-stream (typical after a timeout-retry).  Real cards
        // don't need this because real hosts don't abort mid-stream,
        // but the MI sdmmc layer does, so drop back to IDLE so the
        // new command is recognised.
        if (state_ == S_RECV_DATA && write_pos_ == 0 &&
            !multi_write_active_ &&
            (mosi & 0xC0) == 0x40) {
            state_ = S_IDLE;
        }
        // Multi-block stream abort.  The host issues CMD12 (or any
        // other command) to terminate a CMD18 stream — for CMD25 the
        // host terminates with 0xFD, but it could also send a CMD12
        // for safety.  Detecting a CMD-frame-start byte while either
        // stream is active aborts the stream and routes the byte
        // through normal command receive below.
        if ((multi_read_active_ || multi_write_active_) &&
            cmd_pos_ == 0 && (mosi & 0xC0) == 0x40 &&
            (state_ == S_SEND_DATA || state_ == S_SEND_RESP ||
             (state_ == S_RECV_DATA && write_pos_ == 0))) {
            multi_read_active_ = false;
            multi_write_active_ = false;
            resp_.clear(); resp_idx_ = 0;
            write_pos_ = 0;
            state_ = S_IDLE;
        }
        switch (state_) {
        case S_IDLE: case S_RECV_CMD:
            if (cmd_pos_ == 0 && (mosi & 0xC0) != 0x40) return 0xFF;
            state_ = S_RECV_CMD;
            cmd_[cmd_pos_++] = mosi;
            if (cmd_pos_ == 6) { process_cmd(); cmd_pos_ = 0; }
            return 0xFF;
        case S_SEND_RESP: case S_SEND_DATA:
            if (resp_idx_ < resp_.size()) {
                uint8_t b = resp_[resp_idx_++];
                if (resp_idx_ >= resp_.size()) {
                    resp_.clear(); resp_idx_ = 0;
                    if (multi_read_active_) {
                        // CMD18 stream: refill with next block (or
                        // terminate at end-of-device).  State is set
                        // inside refill_multi_read().
                        refill_multi_read();
                    } else {
                        state_ = post_resp_state_;
                        post_resp_state_ = S_IDLE;
                    }
                }
                return b;
            }
            state_ = S_IDLE; return 0xFF;
        case S_RECV_DATA:
            if (multi_write_active_) {
                // CMD25 multi-block write framing differs from CMD24:
                //   - pad bytes (0xFF) before each block are skipped
                //   - 0xFC token: start of a block (vs 0xFE for CMD24)
                //   - 0xFD token: stop transmission
                // After each block we queue [0x05 response, 0x00 busy,
                // 0xFF release] so pmci_wait_busy_strict (which
                // requires seeing busy before exit) is satisfied.
                if (write_pos_ == 0) {
                    if (mosi == 0xFD) {
                        // Stop tran: brief busy, then idle.
                        multi_write_active_ = false;
                        resp_.push_back(0x00);
                        resp_.push_back(0xFF);
                        state_ = S_SEND_DATA;
                        return 0xFF;
                    }
                    if (mosi == 0xFC) {
                        wbuf_[write_pos_++] = mosi;
                        return 0xFF;
                    }
                    return 0xFF;   // pad byte
                }
                wbuf_[write_pos_++] = mosi;
                if (write_pos_ == 515) {
                    if (img_ && multi_lba_ < total_sectors_) {
                        fseek(img_, (long)multi_lba_ * 512L, SEEK_SET);
                        fwrite(&wbuf_[1], 1, 512, img_); fflush(img_);
                    }
                    multi_lba_++;
                    write_pos_ = 0;
                    resp_.push_back(0x05);   // data accepted
                    resp_.push_back(0x00);   // brief busy
                    resp_.push_back(0xFF);   // release
                    state_ = S_SEND_RESP;
                    post_resp_state_ = S_RECV_DATA;
                    return 0xFF;
                }
                return 0xFF;
            }
            // Single-block CMD24 path:
            // Skip leading 0xFF pad bytes (Nwr gap between R1 and
            // the data token).  Start accumulating once the 0xFE
            // data-start token arrives.
            if (write_pos_ == 0 && mosi != 0xFE) return 0xFF;
            wbuf_[write_pos_++] = mosi;
            if (write_pos_ == 515) {
                flush_write();
                // Queue the data-response token to be returned on
                // the next exchange, not inline with the final CRC
                // byte.  Real cards leave >= 1 byte (Ncrc) of 0xFF
                // between CRC and response; our driver polls.
                resp_.push_back(0x05);
                state_ = S_SEND_RESP;
                return 0xFF;
            }
            return 0xFF;
        }
        return 0xFF;
    }
private:
    enum State { S_IDLE, S_RECV_CMD, S_SEND_RESP, S_SEND_DATA, S_RECV_DATA };
    uint32_t cmd_arg() const {
        return ((uint32_t)cmd_[1]<<24)|((uint32_t)cmd_[2]<<16)|
               ((uint32_t)cmd_[3]<<8)|(uint32_t)cmd_[4];
    }
    void process_cmd() {
        resp_.clear(); resp_idx_ = 0;
        bool prev_app = app_cmd_; app_cmd_ = false;
        state_ = S_SEND_RESP;
        post_resp_state_ = S_IDLE;
        uint8_t cmd = cmd_[0] & 0x3F;
        switch (cmd) {
        case 0: init_ = false; resp_.push_back(0x01); break;
        case 8: resp_.push_back(0x01);
            for (int i=1;i<5;i++) resp_.push_back(cmd_[i]);
            break;
        case 6: { // CMD6 / SD_SEND_SWITCH_FUNC — 64-byte function status.
            // Advertises Group 1 = SDR12 (default) only.  MI sdmmc
            // calls this in mode 0 (query) if the card reports SCR
            // sd_spec >= 1.10 and CCC has SWITCH class.  With only
            // bit 0 of Group 1 set, select_transfer_mode picks
            // best_func = 0 and skips the mode-1 follow-up.
            if (!init_) { resp_.push_back(0x05); break; }
            resp_.push_back(0x00); resp_.push_back(0xFF); resp_.push_back(0xFE);
            uint8_t sfs[64] = {};
            sfs[13] = 0x01;  // Group 1 support bitmap: SDR12 only
            for (int i=0;i<64;i++) resp_.push_back(sfs[i]);
            resp_.push_back(0); resp_.push_back(0);
            state_ = S_SEND_DATA; break;
        }
        case 55: app_cmd_ = true; resp_.push_back(init_?0x00:0x01); break;
        case 41:
            if (!prev_app) { resp_.push_back(0x04); break; }
            init_ = true; resp_.push_back(0x00); break;
        case 51: { // ACMD51 (SD_SEND_SCR): 8-byte SD Configuration Register.
            // MI sdmmc queries this during enumeration to learn spec
            // version / bus widths.  Structure-version must be 0 or 1
            // to pass sdmmc_mem_decode_scr; everything else is advisory.
            if (!prev_app) { resp_.push_back(0x04); break; }
            if (!init_)    { resp_.push_back(0x05); break; }
            resp_.push_back(0x00); resp_.push_back(0xFF); resp_.push_back(0xFE);
            uint8_t scr[8] = {
                0x02,  // [0] SCR_STRUCTURE=0, SD_SPEC=2 (v2.00)
                0x01,  // [1] 1-bit bus width only, no security
                0x00,  // [2] no SPEC3/SPEC4
                0x00,  // [3] no CMD_SUPPORT bits
                0,0,0,0,  // [4..7] manufacturer reserved
            };
            for (int i=0;i<8;i++) resp_.push_back(scr[i]);
            resp_.push_back(0); resp_.push_back(0);
            state_ = S_SEND_DATA; break;
        }
        case 9: { // CSD
            if (!init_) { resp_.push_back(0x05); break; }
            resp_.push_back(0x00); resp_.push_back(0xFF); resp_.push_back(0xFE);
            uint8_t csd[16] = {};
            csd[0]=0x40; csd[1]=0x0E; csd[3]=0x32; csd[4]=0x5B; csd[5]=0x59;
            uint32_t csz = total_sectors_>1024 ? total_sectors_/1024-1 : 0;
            csd[7]=(csz>>16)&0x3F; csd[8]=(csz>>8)&0xFF; csd[9]=csz&0xFF;
            csd[10]=0x7F; csd[11]=0x80; csd[12]=0x0A; csd[13]=0x40; csd[15]=0x01;
            for (int i=0;i<16;i++) resp_.push_back(csd[i]);
            resp_.push_back(0); resp_.push_back(0);
            state_ = S_SEND_DATA; break;
        }
        case 10: { // CID — 16-byte manufacturer identification register.
            // Values are arbitrary but have to be structurally valid so
            // the MI sdmmc_decode_cid path can extract non-empty strings
            // for the boot banner.  Any real card would fill this with
            // manufacturer-supplied data; the ISS forges a stable
            // identity that identifies transfers as simulator-sourced.
            if (!init_) { resp_.push_back(0x05); break; }
            resp_.push_back(0x00); resp_.push_back(0xFF); resp_.push_back(0xFE);
            uint8_t cid[16] = {
                0x03,                    // [ 0] MID: SanDisk-ish
                'P', 'S',                // [ 1.. 2] OID: "PS"
                'I','S','S','_','_',     // [ 3.. 7] PNM: "ISS__"
                0x10,                    // [ 8] PRV: v1.0
                0x00,0x00,0x00,0x01,     // [ 9..12] PSN: serial 1
                0x01, 0x64,              // [13..14] MDT: 2026/04
                0x01,                    // [15] CRC7 + end bit
            };
            for (int i=0;i<16;i++) resp_.push_back(cid[i]);
            resp_.push_back(0); resp_.push_back(0);
            state_ = S_SEND_DATA; break;
        }
        case 58: resp_.push_back(init_?0x00:0x01);
            resp_.push_back(0x40); resp_.push_back(0xFF);
            resp_.push_back(0x80); resp_.push_back(0x00); break;
        case 17: {
            if (!init_) { resp_.push_back(0x05); break; }
            if (cmd_arg() >= total_sectors_) { resp_.push_back(0x20); break; }
            resp_.push_back(0x00); resp_.push_back(0xFF);
            resp_.push_back(0xFF); resp_.push_back(0xFE);
            uint8_t sec[512]; memset(sec, 0xFF, 512);
            fseek(img_, (long)cmd_arg()*512L, SEEK_SET);
            if (fread(sec, 1, 512, img_) < 512) memset(sec, 0xFF, 512);
            for (int i=0;i<512;i++) resp_.push_back(sec[i]);
            resp_.push_back(0); resp_.push_back(0);
            state_ = S_SEND_DATA; break;
        }
        case 24:
            if (!init_) { resp_.push_back(0x05); break; }
            if (cmd_arg() >= total_sectors_) { resp_.push_back(0x20); break; }
            // R1 must drain before we start receiving data; stay in
            // S_SEND_RESP until the host reads the response byte,
            // then transition to S_RECV_DATA for the data phase.
            resp_.push_back(0x00); write_lba_=cmd_arg(); write_pos_=0;
            post_resp_state_ = S_RECV_DATA; break;
        case 12:
            // STOP_TRANSMISSION.  The host has already aborted the
            // multi-block stream by sending this CMD frame (which we
            // detected in the abort path above before re-entering
            // process_cmd), so all we need to do here is acknowledge.
            // Cleared streaming flags belong to the abort path, not
            // here — by the time process_cmd runs, both flags are
            // already false.
            resp_.push_back(0x00);
            break;
        case 18: {
            // READ_BLOCK_MULTIPLE.  Stream blocks until the host
            // sends CMD12.  Pre-load R1 + gap + first block into
            // resp_; refill_multi_read() refills with subsequent
            // blocks as resp_ drains.
            if (!init_) { resp_.push_back(0x05); break; }
            if (cmd_arg() >= total_sectors_) { resp_.push_back(0x20); break; }
            resp_.push_back(0x00); resp_.push_back(0xFF);
            multi_read_active_ = true;
            multi_lba_ = cmd_arg();
            push_read_block(multi_lba_++);
            state_ = S_SEND_DATA;
            break;
        }
        case 23:
            // ACMD23 (SET_WR_BLK_ERASE_COUNT) or CMD23 (SET_BLOCK_COUNT).
            // Either is just a hint — we don't track block count.  R1
            // accept and move on.  The MI sdmmc layer issues ACMD23
            // before CMD25 in SD mode, and the write would fail
            // entirely if we returned "illegal command" here.
            resp_.push_back(init_ ? 0x00 : 0x05);
            break;
        case 25:
            // WRITE_BLOCK_MULTIPLE.  Like CMD24 but in S_RECV_DATA
            // we accept 0xFC tokens for blocks and 0xFD for stop.
            if (!init_) { resp_.push_back(0x05); break; }
            if (cmd_arg() >= total_sectors_) { resp_.push_back(0x20); break; }
            resp_.push_back(0x00);
            multi_write_active_ = true;
            multi_lba_ = cmd_arg();
            write_pos_ = 0;
            post_resp_state_ = S_RECV_DATA;
            break;
        default: resp_.push_back(0x04); break;
        }
    }

    // Push 0xFE + 512 data bytes + 2 CRC for the given LBA.  Used by
    // CMD17 (single) initial inline; here as a helper for CMD18 refill.
    void push_read_block(uint32_t lba) {
        resp_.push_back(0xFE);
        uint8_t sec[512]; memset(sec, 0xFF, 512);
        if (img_ && lba < total_sectors_) {
            fseek(img_, (long)lba * 512L, SEEK_SET);
            if (fread(sec, 1, 512, img_) < 512) memset(sec, 0xFF, 512);
        }
        for (int i = 0; i < 512; i++) resp_.push_back(sec[i]);
        resp_.push_back(0); resp_.push_back(0);
    }

    // Refill resp_ with the next block of a multi-read stream, or
    // terminate the stream if we've run off the end of the device.
    void refill_multi_read() {
        resp_.clear(); resp_idx_ = 0;
        if (multi_lba_ >= total_sectors_) {
            multi_read_active_ = false;
            state_ = S_IDLE;
            return;
        }
        push_read_block(multi_lba_++);
        state_ = S_SEND_DATA;
    }
    void flush_write() {
        if (img_ && write_lba_ < total_sectors_) {
            fseek(img_, (long)write_lba_*512L, SEEK_SET);
            fwrite(&wbuf_[1], 1, 512, img_); fflush(img_);
        }
        state_ = S_IDLE; write_pos_ = 0;
    }
    FILE* img_ = nullptr; uint32_t total_sectors_ = 0;
    bool selected_=false, init_=false, app_cmd_=false;
    State state_ = S_IDLE;
    State post_resp_state_ = S_IDLE;
    uint8_t cmd_[6]={}; int cmd_pos_=0;
    std::vector<uint8_t> resp_; size_t resp_idx_=0;
    uint8_t wbuf_[515]={}; int write_pos_=0; uint32_t write_lba_=0;
    /* Multi-block (CMD18 read, CMD25 write) state.  These are set
     * in process_cmd() and cleared either when the host issues
     * CMD12 / 0xFD stop-tran, or when the block stream reaches
     * end-of-device. */
    bool multi_read_active_ = false;
    bool multi_write_active_ = false;
    uint32_t multi_lba_ = 0;
};

// ═══════════════════════════════════════════════════════════════
// Constants
// ═══════════════════════════════════════════════════════════════

// SR bit positions
static constexpr uint32_t SR_N = 1u << 0;
static constexpr uint32_t SR_Z = 1u << 1;
static constexpr uint32_t SR_C = 1u << 2;
static constexpr uint32_t SR_V = 1u << 3;
static constexpr uint32_t SR_I = 1u << 30;
static constexpr uint32_t SR_S = 1u << 31;
static constexpr uint32_t SR_FLAGS = SR_N|SR_Z|SR_C|SR_V;

// Exception vectors
enum { VEC_BUS_FAULT=0, VEC_TIMER=1, VEC_TLB_MISS=2, VEC_TLB_PROT=3,
       VEC_PRIV=4, VEC_SYSCALL=5, VEC_BREAK=6, VEC_ILLEGAL=7, VEC_ALIGN=8,
       VEC_EXT_IRQ=9 };

// Sysreg device IDs
enum { SYSDEV_MMU=0, SYSDEV_CPU=1, SYSDEV_DCACHE=2, SYSDEV_ICACHE=3, SYSDEV_BUS=4,
       SYSDEV_TIMER=7, SYSDEV_MACH=8, SYSDEV_DEBUG=15 };

// Debug watchpoints — halt on physical memory write to watched addresses
static constexpr int DBG_MAX_WATCH = 16;
static struct {
    uint32_t watch_pa[DBG_MAX_WATCH];
    int      watch_count;
    uint32_t watch_val;    // Only trigger when this value is written (0xFFFFFFFF = any)
} dbg;

// MMU sysreg addresses
enum { MMU_CR=0, MMU_FADDR=1, MMU_FSTAT=2, MMU_TLB_VPN=3, MMU_TLB_PTE=4, MMU_TLB_IDX=5 };

// TLB PTE flag masks (from flag byte in PTE word bits [7:0])
static constexpr uint32_t TLB_V = 1u << 0;   // Valid
static constexpr uint32_t TLB_C = 1u << 2;   // Cacheable
static constexpr uint32_t TLB_R = 1u << 3;   // Read
static constexpr uint32_t TLB_W = 1u << 4;   // Write
static constexpr uint32_t TLB_X = 1u << 5;   // Execute
static constexpr uint32_t TLB_U = 1u << 6;   // User-accessible
static constexpr uint32_t TLB_G = 1u << 7;   // Global (skip ASID)

// TLB field extraction helpers
static inline uint32_t TLB_VPN_GET_VPN(uint32_t vpn_word)  { return (vpn_word >> 8) & 0xFFFFF; }
static inline uint8_t  TLB_VPN_GET_ASID(uint32_t vpn_word) { return vpn_word & 0xFF; }
static inline uint32_t TLB_PTE_GET_PPN(uint32_t pte_word)  { return pte_word >> 12; }

// SPR numbers (in IR[15:12])
enum { SPR_ESR=0, SPR_EPC=1, SPR_USP=2, SPR_SR=3, SPR_SCR0=4 };
constexpr int N_SCR = 4;   // SCR0..SCR(N_SCR-1) at SPR_SCR0..SPR_SCR0+N_SCR-1

// Memory sizes
static constexpr size_t RAM_SIZE = 16u * 1024 * 1024;  // 16 MB
static constexpr size_t ROM_SIZE = 64u * 1024;          // 64 KB
static constexpr uint32_t RAM_BASE  = 0x00000000;
static constexpr uint32_t UART_BASE = 0xFF000000;
static constexpr uint32_t ROM_BASE  = 0xFFFF0000;
static constexpr uint32_t ACFG_BASE = 0xFE000000;

// ═══════════════════════════════════════════════════════════════
// CPU State
// ═══════════════════════════════════════════════════════════════

struct CPU {
    uint32_t r[16];     // R0 = hardwired zero, R14 = current SP, R15 unused (use pc)
    uint32_t pc;
    uint32_t sr;
    uint32_t esr, epc;
    uint32_t usp;       // Banked SP (user SP when in supervisor, supervisor SP when in user)
    uint32_t scr[N_SCR]; // Scratch SPRs SCR0..SCR(N_SCR-1) — supervisor-only storage
    uint64_t insn_count;
    bool halted;
    bool ei_shadow;     // One-instruction delay after EI
};

static CPU cpu;

// ═══════════════════════════════════════════════════════════════
// Memory and Devices
// ═══════════════════════════════════════════════════════════════

static uint8_t ram[RAM_SIZE];
static uint8_t rom[ROM_SIZE];

// --- UART (16450-compatible) ---
static struct {
    uint8_t rbr, ier, lcr, mcr, scr, dll, dlm;
    bool    rx_ready;
    bool    rx_break;       // LSR.BI sticky bit; cleared on LSR read
    bool    thre_int;
    int     tx_busy_count;  // counts down per instruction; 0 = ready
    bool    dlab() const { return lcr & 0x80; }
    bool    tx_ready() const { return tx_busy_count == 0; }
    uint8_t lsr() const {
        return (uint8_t)((tx_ready()<<6)|(tx_ready()<<5)|
                         (rx_break ? 0x10 : 0)|rx_ready);
    }
    uint8_t msr() const { return 0x30; } // CTS+DSR hardwired
    uint8_t iir() const {
        if ((ier&1) && rx_ready) return 0x04;
        if ((ier&2) && thre_int) return 0x02;
        return 0x01;
    }
    bool    irq() const { return ((iir() & 1) == 0) && (mcr & 0x08); }
} uart;

// --- SPI controller (v2 register interface) ---
// ISS is instruction-level — transfers complete instantly (no cycle delay).
// FIFO mode is supported for register compatibility but the engine runs
// the full transfer synchronously on the XFER_COUNT write.
static constexpr int ISS_SPI_FIFO_DEPTH = 512;

static struct {
    uint8_t  rx_data;       // Single-byte mode RX latch
    uint16_t control;       // CS, mode, speed, FIFO_EN
    bool     done;          // Single-byte DONE flag

    // FIFO state (circular buffers)
    uint8_t  tx_fifo[ISS_SPI_FIFO_DEPTH];
    uint8_t  rx_fifo[ISS_SPI_FIFO_DEPTH];
    int      tx_head, tx_tail, tx_count;
    int      rx_head, rx_tail, rx_count;

    // IRQ state
    uint8_t  irq_enable;
    bool     xfer_done;
    uint16_t eng_count;     // Remaining count (for read-back)

    bool fifo_en() const { return (control >> 7) & 1; }

    void tx_push(uint8_t v) {
        if (tx_count < ISS_SPI_FIFO_DEPTH) {
            tx_fifo[tx_tail] = v;
            tx_tail = (tx_tail + 1) % ISS_SPI_FIFO_DEPTH;
            tx_count++;
        }
    }
    uint8_t tx_pop() {
        if (tx_count == 0) return 0xFF;
        uint8_t v = tx_fifo[tx_head];
        tx_head = (tx_head + 1) % ISS_SPI_FIFO_DEPTH;
        tx_count--;
        return v;
    }
    void rx_push(uint8_t v) {
        if (rx_count < ISS_SPI_FIFO_DEPTH) {
            rx_fifo[rx_tail] = v;
            rx_tail = (rx_tail + 1) % ISS_SPI_FIFO_DEPTH;
            rx_count++;
        }
    }
    uint8_t rx_pop() {
        if (rx_count == 0) return 0xFF;
        uint8_t v = rx_fifo[rx_head];
        rx_head = (rx_head + 1) % ISS_SPI_FIFO_DEPTH;
        rx_count--;
        return v;
    }
    void tx_flush() { tx_head = tx_tail = tx_count = 0; }
    void rx_flush() { rx_head = rx_tail = rx_count = 0; }

    bool rx_thresh() const { return rx_count >= ISS_SPI_FIFO_DEPTH / 2; }
    bool tx_thresh() const { return tx_count <= ISS_SPI_FIFO_DEPTH / 2; }

    void reset() {
        rx_data = 0xFF; control = 0x03; done = false;
        tx_flush(); rx_flush();
        irq_enable = 0; xfer_done = false; eng_count = 0;
    }

    uint32_t read_reg(int reg) const {
        switch (reg) {
            case 0: // CAP
                return (ISS_SPI_FIFO_DEPTH << 8) | 1;
            case 1: { // STATUS
                uint32_t s = done ? 0x02 : 0x00;
                if (fifo_en()) {
                    s |= ((uint32_t)tx_count << 4);
                    s |= ((uint32_t)rx_count << 16);
                    if (tx_count == 0)                s |= (1u << 28);
                    if (tx_count == ISS_SPI_FIFO_DEPTH) s |= (1u << 29);
                    if (rx_count == 0)                s |= (1u << 30);
                    if (rx_count == ISS_SPI_FIFO_DEPTH) s |= (1u << 31);
                }
                return s;
            }
            case 2: return control;
            case 3: // DATA
                return 0; // Read handled in spi_read() for pop side-effect
            case 4: return eng_count;
            case 5: { // IRQ_STATUS
                uint32_t s = xfer_done ? 1 : 0;
                if (rx_thresh()) s |= 2;
                if (tx_thresh()) s |= 4;
                return s;
            }
            case 6: return irq_enable;
            default: return 0;
        }
    }
} spi;

static SdCardSim* sd_card = nullptr;

// --- Timer (sysreg device 7) ---
// Simulates a 16-bit countdown timer ticking at TICK_FREQ_HZ.
// In the ISS, one "tick" occurs every TIMER_PRESCALE instructions
// (approximating the cycle-based prescaler in RTL).
// With a 25 MHz assumed CPU clock and 1 MHz timer, that's ÷25.
// Since ISS instructions average ~1 cycle, prescale=25 is reasonable.
static constexpr int      TIMER_PRESCALE  = 25;
static constexpr uint32_t TIMER_FREQ_HZ   = 1000000;

static struct {
    // Sysreg registers
    uint16_t count;
    uint16_t reload;
    bool     tick_en;
    bool     irq_en;
    bool     autoload;
    bool     udf;          // Underflow flag

    // Internal prescaler
    int      prescale_cnt;

    // Timer sysreg register indices
    enum { TM_FREQ=0, TM_CR=1, TM_COUNT=2, TM_RELOAD=3, TM_STATUS=4 };

    void reset() {
        count = 0; reload = 0;
        tick_en = false; irq_en = false; autoload = false;
        udf = false; prescale_cnt = 0;
    }

    bool irq() const { return udf && irq_en; }

    // Called once per ISS instruction
    void step() {
        if (!tick_en) return;
        if (++prescale_cnt < TIMER_PRESCALE) return;
        prescale_cnt = 0;

        // One timer tick
        if (count == 0) {
            // Underflow
            udf = true;
            if (autoload)
                count = reload;
            else
                tick_en = false;
        } else {
            count--;
        }
    }

    uint32_t read_reg(int reg) const {
        switch (reg) {
            case TM_FREQ:   return TIMER_FREQ_HZ;
            case TM_CR:     return (autoload ? 4u : 0) | (irq_en ? 2u : 0) | (tick_en ? 1u : 0);
            case TM_COUNT:  return count;
            case TM_RELOAD: return reload;
            case TM_STATUS: return udf ? 1u : 0u;
            default:         return 0;
        }
    }

    void write_reg(int reg, uint32_t val) {
        switch (reg) {
            case TM_CR:
                tick_en  = val & 1;
                irq_en   = (val >> 1) & 1;
                autoload = (val >> 2) & 1;
                break;
            case TM_COUNT:
                count = (uint16_t)val;
                break;
            case TM_RELOAD:
                reload = (uint16_t)val;
                break;
            case TM_STATUS:
                if (val & 1) udf = false;  // Write-1-to-clear
                break;
        }
    }
} timer;

// --- Bus controller (sysreg device 4) ---
static struct {
    uint32_t reg;   // bit 0 = RST, bit 1 = CFG_EN
    bool rst()    const { return reg & 1; }
    bool cfg_en() const { return reg & 2; }
} busctl;

// --- Autoconfig device (SPI/SD) ---
static struct {
    bool     configured;
    uint32_t base_addr;
    bool     cfg_seen_low;  // For chain propagation

    void reset() { configured=false; base_addr=0; cfg_seen_low=false; }
    bool cfg_active(bool cfg_en, uint32_t addr) const {
        return cfg_en && !configured && (addr >> 5) == (ACFG_BASE >> 5);
    }
    bool dev_sel(uint32_t addr) const {
        return configured && ((addr & ~0xFFFu) == base_addr);
    }
} acfg;

// --- TLB (64 entries: 32 sets × 2 ways) ---
static struct {
    uint32_t vpn[64];   // {4'b0, VPN[19:0], ASID[7:0]}
    uint32_t pte[64];   // {PPN[19:0], SW[3:0], flags[7:0]}
} tlb;

// --- Pinned TLB (4 entries, fully associative) ---
static constexpr int PTLB_ENTRIES = 4;
static struct {
    uint32_t vpn[PTLB_ENTRIES];
    uint32_t pte[PTLB_ENTRIES];
} ptlb;

// --- MMU state ---
static struct {
    uint32_t cr;          // [0]=M enable, [15:8]=ASID
    uint32_t fault_addr;
    uint32_t fault_status;
    uint32_t tlb_idx;     // [6]=pinned, [5]=way, [4:0]=set (or [1:0]=pin slot)
    uint32_t tlb_vpn_reg; // Shared staging register
    bool     enabled() const { return cr & 1; }
    uint8_t  asid()    const { return (cr >> 8) & 0xFF; }
} mmu;

// --- Sysreg: System ID (device 1, read-only) ---
static uint32_t cpuid_read(int reg) {
    static const uint32_t cpu_name[4] = {0x756E6550,0x6172626D,0x0000312F,0};
    switch (reg) {
        case 0: return 1;            // CPU_ISA: version 1
        case 1: case 2: case 3: case 4: return cpu_name[reg-1];
        // The ISS is instruction-accurate, not cycle-accurate.  We expose
        // insn_count for both registers so the bench harness can read
        // them without special-casing — IPC will always be 1 on the ISS,
        // which is clearly synthetic.  Real cycle numbers come from RTL
        // simulation or FPGA execution.
        case 5: return (uint32_t)cpu.insn_count;  // CPU_CYCLES (synthetic on ISS)
        case 6: return (uint32_t)cpu.insn_count;  // CPU_INSNS_RETIRED
        default: return 0;
    }
}

static uint32_t machid_read(int reg) {
    static const uint32_t mach_name[4] = {0x756D6953,0x6F74616C,0x00000072,0};
    switch (reg) {
        case 0: return 0;            // MACH_FEAT
        case 1: case 2: case 3: case 4: return mach_name[reg-1];
        case 5: return 25000000;     // CPU_FREQ: 25 MHz (simulated)
        default: return 0;
    }
}

// ═══════════════════════════════════════════════════════════════
// Terminal and signal handling
// ═══════════════════════════════════════════════════════════════

static volatile sig_atomic_t running = 1;
static struct termios orig_termios;
static bool term_raw = false;
static bool full_raw = false;  // +raw: pass all control chars through
static bool halt_on_break = false;  // +halt-on-break: BREAK halts (testbench mode)
static bool hosted_mode = false;  // +hosted: native syscall interception
static bool quiet_mode = false;   // +quiet: suppress banner/exit messages
static int  hosted_exit_code = 0; // return code from SYS_exit in hosted mode
static uint64_t max_insns = 0;    // +max-insn=N: halt after N instructions
static bool trap_user_pc_zero = false;  // +trap-pc0: abort on user-mode PC=0
static FILE* trace_fp = nullptr;

static void sigint_handler(int) { running = 0; }
static void restore_term() {
    if (term_raw) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); term_raw = false; }
}
static void raw_mode() {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_term);
    struct termios raw = orig_termios;
    if (full_raw) {
        // Full raw: clear everything so Ctrl-C/Z/\ pass through to guest
        raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                         INLCR | IGNCR | ICRNL | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    } else {
        raw.c_lflag &= ~(ECHO | ICANON);
    }
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    term_raw = true;
}

// ═══════════════════════════════════════════════════════════════
// Helper: register read/write
// ═══════════════════════════════════════════════════════════════

static inline uint32_t reg_read(int r) {
    if (r == 0) return 0;
    if (r == 15) return cpu.pc;
    return cpu.r[r];
}

static inline void reg_write(int r, uint32_t val) {
    if (r == 0 || r == 15) return;  // R0 hardwired, R15 not writable via ALU
    cpu.r[r] = val;
}

// SP banking: swap r[14] and usp on privilege mode change
static void bank_sp(uint32_t old_sr, uint32_t new_sr) {
    if ((old_sr ^ new_sr) & SR_S) {
        uint32_t tmp = cpu.r[14];
        cpu.r[14] = cpu.usp;
        cpu.usp = tmp;
    }
}

// ═══════════════════════════════════════════════════════════════
// TLB Lookup
// ═══════════════════════════════════════════════════════════════

// Perform a TLB lookup for the given virtual address.
// access: bit 0=R, bit 1=W, bit 2=X (one-hot, matching TLB flag positions)
// Returns true on hit (paddr/cacheable valid), false on miss.
// Sets fault=true if hit but permissions deny access.
static bool tlb_lookup(uint32_t vaddr, uint8_t asid, uint8_t access,
                       bool user_mode, uint32_t& paddr, bool& cacheable,
                       bool& fault) {
    uint32_t va_vpn = vaddr >> 12;

    // Build access permission mask from one-hot access type:
    //   access bit 0 (read)  → TLB_R
    //   access bit 1 (write) → TLB_W
    //   access bit 2 (exec)  → TLB_X
    uint32_t need_mask = 0;
    if (access & 1) need_mask |= TLB_R;
    if (access & 2) need_mask |= TLB_W;
    if (access & 4) need_mask |= TLB_X;
    if (user_mode)  need_mask |= TLB_U;

    // Check pinned TLB first (fully associative, priority over main)
    for (int i = 0; i < PTLB_ENTRIES; i++) {
        uint32_t pte_word = ptlb.pte[i];
        if (!(pte_word & TLB_V)) continue;

        uint32_t entry_vpn  = TLB_VPN_GET_VPN(ptlb.vpn[i]);
        uint8_t  entry_asid = TLB_VPN_GET_ASID(ptlb.vpn[i]);
        bool     global     = pte_word & TLB_G;

        if (entry_vpn != va_vpn) continue;
        if (!global && entry_asid != asid) continue;

        fault = (pte_word & need_mask) != need_mask;
        paddr = (TLB_PTE_GET_PPN(pte_word) << 12) | (vaddr & 0xFFF);
        cacheable = pte_word & TLB_C;
        return true;
    }

    // Fall through to main TLB (set-associative)
    int set = va_vpn & 0x1F;
    for (int way = 0; way < 2; way++) {
        int idx = way * 32 + set;
        uint32_t vpn_word = tlb.vpn[idx];
        uint32_t pte_word = tlb.pte[idx];

        if (!(pte_word & TLB_V)) continue;

        uint32_t entry_vpn  = TLB_VPN_GET_VPN(vpn_word);
        uint8_t  entry_asid = TLB_VPN_GET_ASID(vpn_word);
        bool     global     = pte_word & TLB_G;

        if (entry_vpn != va_vpn) continue;
        if (!global && entry_asid != asid) continue;

        fault = (pte_word & need_mask) != need_mask;
        paddr = (TLB_PTE_GET_PPN(pte_word) << 12) | (vaddr & 0xFFF);
        cacheable = pte_word & TLB_C;
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
// MMU Translation
// ═══════════════════════════════════════════════════════════════

// Returns: 0=ok, VEC_TLB_MISS, VEC_TLB_PROT, VEC_ALIGN, VEC_BUS_FAULT
static int mmu_translate(uint32_t vaddr, uint8_t access, int size,
                         bool force_bypass, uint32_t& paddr) {
    // Alignment check (always, even in bypass)
    if ((size == 2 && (vaddr & 3)) || (size == 1 && (vaddr & 1))) {
        mmu.fault_addr = vaddr;
        bool user = !(cpu.sr & SR_S);
        mmu.fault_status = ((uint32_t)user << 11) | ((uint32_t)access << 8) | 0x03;
        return VEC_ALIGN;
    }

    if (!mmu.enabled() || force_bypass) {
        paddr = vaddr;
        return 0;
    }

    bool cacheable, fault;
    bool hit = tlb_lookup(vaddr, mmu.asid(), access, !(cpu.sr & SR_S),
                          paddr, cacheable, fault);
    if (!hit) {
        mmu.fault_addr = vaddr;
        mmu.fault_status = (uint32_t)((!(cpu.sr&SR_S))<<11) | ((uint32_t)access<<8) | 0x01;
        return VEC_TLB_MISS;
    }
    if (fault) {
        mmu.fault_addr = vaddr;
        mmu.fault_status = (uint32_t)((!(cpu.sr&SR_S))<<11) | ((uint32_t)access<<8) | 0x02;
        return VEC_TLB_PROT;
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════
// Physical Memory Access
// ═══════════════════════════════════════════════════════════════

static inline uint32_t rd8(const uint8_t* p) { return *p; }
static inline uint32_t rd16(const uint8_t* p) { return p[0] | (p[1]<<8); }
static inline uint32_t rd32(const uint8_t* p) { return p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24); }
static inline void wr8(uint8_t* p, uint32_t v) { p[0]=v&0xFF; }
static inline void wr16(uint8_t* p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static inline void wr32(uint8_t* p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

// UART register read
static uint32_t uart_read(uint32_t addr) {
    int reg = (addr >> 2) & 7;
    switch (reg) {
        case 0: if (uart.dlab()) return uart.dll;
                { uint8_t b = uart.rbr; uart.rx_ready = false; return b; }
        case 1: return uart.dlab() ? uart.dlm : uart.ier;
        case 2: {
            uint8_t v = uart.iir();
            // Reading IIR with THRE shown clears thre_int
            if (v == 0x02) uart.thre_int = false;
            return v;
        }
        case 3: return uart.lcr;
        case 4: return uart.mcr;
        case 5: {
            uint8_t v = uart.lsr();
            uart.rx_break = false;  /* LSR.BI clears on LSR read */
            return v;
        }
        case 6: return uart.msr();
        case 7: return uart.scr;
    }
    return 0;
}

// UART register write
static void uart_write(uint32_t addr, uint32_t data) {
    int reg = (addr >> 2) & 7;
    uint8_t val = data & 0xFF;
    switch (reg) {
        case 0:
            if (uart.dlab()) { uart.dll = val; }
            else { putchar(val); fflush(stdout); uart.thre_int = false; uart.tx_busy_count = 2; }
            break;
        case 1: if (uart.dlab()) uart.dlm = val; else uart.ier = val; break;
        case 2: break; // FCR: ignored
        case 3: uart.lcr = val; break;
        case 4: uart.mcr = val; break;
        case 7: uart.scr = val; break;
    }
}

// SPI single-byte exchange via SD card emulator
static uint8_t spi_exchange(uint8_t mosi) {
    if (sd_card) {
        sd_card->select(!(spi.control & 1)); // CS0 active-low
        return sd_card->exchange(mosi);
    }
    return 0xFF;
}

// SPI register write (v2 register interface)
static void spi_write(int reg, uint32_t data) {
    switch (reg) {
        case 2: // CONTROL
            spi.control = data & 0x3FFF;
            if (data & (1 << 14)) spi.tx_flush(); // FLUSH_TX
            if (data & (1 << 15)) spi.rx_flush(); // FLUSH_RX
            break;
        case 3: { // DATA
            uint8_t mosi = data & 0xFF;
            if (spi.fifo_en()) {
                spi.tx_push(mosi);
            } else {
                // Single-byte mode: instant transfer
                spi.rx_data = spi_exchange(mosi);
                spi.done = true;
            }
            break;
        }
        case 4: // XFER_COUNT
            if (data & (1 << 16) && spi.fifo_en()) {
                // START — run transfer synchronously in ISS
                uint16_t count = data & 0xFFFF;
                for (uint16_t i = 0; i < count; i++) {
                    uint8_t tx = spi.tx_pop(); // 0xFF if empty
                    uint8_t rx = spi_exchange(tx);
                    spi.rx_push(rx);
                }
                spi.eng_count = 0;
                spi.xfer_done = true;
            }
            break;
        case 5: // IRQ_STATUS (W1C)
            if (data & 1) spi.xfer_done = false;
            break;
        case 6: // IRQ_ENABLE
            spi.irq_enable = data & 0x07;
            break;
        // case 0 (CAP) and case 1 (STATUS) are read-only
    }
}

// Autoconfig config space read
static uint32_t acfg_config_read(uint32_t addr) {
    int reg = (addr >> 2) & 7;
    switch (reg) {
        case 0: return 4;       // CLASS_SD
        case 1: return 4096;    // SIZE
        case 2: return 0;       // ID
        case 3: return 0x00004453; // "SD\0\0" LE
        case 4: case 5: case 6: return 0;
        case 7: return 0;       // BASE (write-only)
    }
    return 0;
}

// Physical memory read. Returns bus_fault=true if unmapped.
static uint32_t phys_read(uint32_t addr, int size, bool& bus_fault) {
    bus_fault = false;

    // RAM: 0x00000000 – 0x00FFFFFF
    if (addr < RAM_SIZE) {
        if (size==0) return rd8(&ram[addr]);
        if (size==1) return rd16(&ram[addr & ~1u]);
        return rd32(&ram[addr & ~3u]);
    }

    // Autoconfig space: 0xFE000000 (when cfg_en and !configured)
    if (acfg.cfg_active(busctl.cfg_en(), addr)) {
        return acfg_config_read(addr);
    }

    // SPI at dynamic base (when configured)
    if (acfg.dev_sel(addr)) {
        int reg = (addr >> 2) & 7;
        if (reg == 3) { // DATA — has pop side-effect
            if (spi.fifo_en())
                return spi.rx_pop();
            else
                return spi.rx_data;
        }
        return spi.read_reg(reg);
    }

    // UART: 0xFF000000 – 0xFF000FFF
    if ((addr >> 12) == (UART_BASE >> 12)) {
        return uart_read(addr);
    }

    // ROM: 0xFFFF0000 – 0xFFFFFFFF
    if (addr >= ROM_BASE) {
        uint32_t off = addr - ROM_BASE;
        if (off < ROM_SIZE) {
            if (size==0) return rd8(&rom[off]);
            if (size==1) return rd16(&rom[off & ~1u]);
            return rd32(&rom[off & ~3u]);
        }
    }

    bus_fault = true;
    return 0;
}

// Physical memory write. Returns bus_fault=true if unmapped.
static void phys_write(uint32_t addr, uint32_t data, int size, bool& bus_fault) {
    bus_fault = false;

    // RAM
    if (addr < RAM_SIZE) {
        // Debug watchpoint: halt on write to watched PA
        if (dbg.watch_count > 0) {
            uint32_t wa = addr & (size==0 ? ~0u : size==1 ? ~1u : ~3u);
            uint32_t wsz = 1u << size;
            for (int wi = 0; wi < dbg.watch_count; wi++) {
                uint32_t wpa = dbg.watch_pa[wi];
                if (wa <= wpa && wa + wsz > wpa) {
                    if (dbg.watch_val == 0xFFFFFFFF ||
                        data == dbg.watch_val) {
                        uint32_t old = rd32(&ram[wpa & ~3u]);
                        printf("\n[WATCHPOINT] PA=0x%08X old=0x%08X "
                               "new=0x%08X sz=%d PC=0x%08X\n",
                               wpa, old, data, 1<<size, cpu.pc);
                        cpu.halted = true;
                    }
                    break;
                }
            }
        }
        if (size==0) wr8(&ram[addr], data);
        else if (size==1) wr16(&ram[addr & ~1u], data);
        else wr32(&ram[addr & ~3u], data);
        return;
    }

    // Autoconfig: write to CFG_BASE assigns base address
    if (acfg.cfg_active(busctl.cfg_en(), addr)) {
        if (((addr >> 2) & 7) == 7) { // CFG_BASE register
            acfg.base_addr = data;
            acfg.configured = true;
        }
        return;
    }

    // SPI at dynamic base
    if (acfg.dev_sel(addr)) {
        spi_write((addr >> 2) & 7, data);
        return;
    }

    // UART
    if ((addr >> 12) == (UART_BASE >> 12)) {
        uart_write(addr, data);
        return;
    }

    // ROM: writes are silently ignored
    if (addr >= ROM_BASE) return;

    bus_fault = true;
}

// ═══════════════════════════════════════════════════════════════
// Sysreg Access (WRSYS/RDSYS)
// ═══════════════════════════════════════════════════════════════

static uint32_t sysreg_read(int dev, int reg) {
    switch (dev) {
    case SYSDEV_MMU:
        switch (reg) {
            case MMU_CR: return mmu.cr;
            case MMU_FADDR: return mmu.fault_addr;
            case MMU_FSTAT: return mmu.fault_status;
            case MMU_TLB_VPN: {
                if (mmu.tlb_idx & 0x40) {
                    return ptlb.vpn[mmu.tlb_idx & (PTLB_ENTRIES - 1)];
                } else {
                    int idx = (mmu.tlb_idx & 0x20 ? 32 : 0) + (mmu.tlb_idx & 0x1F);
                    return tlb.vpn[idx];
                }
            }
            case MMU_TLB_PTE: {
                if (mmu.tlb_idx & 0x40) {
                    return ptlb.pte[mmu.tlb_idx & (PTLB_ENTRIES - 1)];
                } else {
                    int idx = (mmu.tlb_idx & 0x20 ? 32 : 0) + (mmu.tlb_idx & 0x1F);
                    return tlb.pte[idx];
                }
            }
            case MMU_TLB_IDX: return mmu.tlb_idx;
            default: return 0;
        }
    case SYSDEV_CPU: return cpuid_read(reg);
    case SYSDEV_DCACHE: case SYSDEV_ICACHE:
        // Unified CACHE_INFO layout: line_words[5:0]|num_sets[20:6]|
        // num_ways[25:21]|addressing[27:26]|wb[28]|wa[29].
        // Report a plausible L1 (16 sets × 1-way × 4 words, PIPT, WT/WnA);
        // the ISS doesn't model timing so the values are advisory.
        if (reg == 0) return (0u << 26) | (1u << 21) | (16u << 6) | 4u;
        return 0;
    case SYSDEV_BUS: return (reg == 0) ? busctl.reg : 0;
    case SYSDEV_TIMER: return timer.read_reg(reg);
    case SYSDEV_MACH: return machid_read(reg);
    default: return 0;
    }
}

static void sysreg_write(int dev, int reg, uint32_t val) {
    switch (dev) {
    case SYSDEV_MMU:
        switch (reg) {
            case MMU_CR: mmu.cr = val; break;
            case MMU_TLB_IDX: mmu.tlb_idx = val; break;
            case MMU_TLB_VPN: mmu.tlb_vpn_reg = val; break;
            case MMU_TLB_PTE: {
                // Writing PTE commits VPN + PTE; TLB_INDEX[6] selects target
                if (mmu.tlb_idx & 0x40) {
                    int i = mmu.tlb_idx & (PTLB_ENTRIES - 1);
                    ptlb.vpn[i] = mmu.tlb_vpn_reg;
                    ptlb.pte[i] = val;
                } else {
                    int idx = (mmu.tlb_idx & 0x20 ? 32 : 0) + (mmu.tlb_idx & 0x1F);
                    tlb.vpn[idx] = mmu.tlb_vpn_reg;
                    tlb.pte[idx] = val;
                }
                break;
            }
        }
        break;
    case SYSDEV_BUS:
        if (reg == 0) {
            busctl.reg = val;
            if (busctl.rst()) { acfg.reset(); spi.reset(); }
            if (!busctl.cfg_en()) acfg.cfg_seen_low = true;
        }
        break;
    case SYSDEV_TIMER:
        timer.write_reg(reg, val);
        break;
    case SYSDEV_DEBUG:
        if (reg == 0) {  // WATCH_PA: add to list (0 = clear all)
            if (val == 0) {
                dbg.watch_count = 0;
                printf("[DEBUG] Watchpoints cleared\n");
            } else if (dbg.watch_count < DBG_MAX_WATCH) {
                dbg.watch_pa[dbg.watch_count++] = val;
                printf("[DEBUG] Watchpoint #%d: PA=0x%08X val=0x%08X\n",
                       dbg.watch_count, val, dbg.watch_val);
            }
        } else if (reg == 1) {  // WATCH_VAL
            dbg.watch_val = val;
        }
        break;
    case SYSDEV_DCACHE: case SYSDEV_ICACHE:
        break; // Cache control: accept and ignore in ISS
    }
}

// ═══════════════════════════════════════════════════════════════
// Exception Entry
// ═══════════════════════════════════════════════════════════════

static bool pc_written;  // Set by exception entry, branches, jumps

static void exception_entry(int vector) {
    if (hosted_mode) {
        fprintf(stderr, "\n[ERROR] Exception %d at PC=0x%08X in hosted mode. Aborting.\n", vector, cpu.pc);
        fprintf(stderr, "  FAULT_ADDR=0x%08X FAULT_STATUS=0x%08X\n", mmu.fault_addr, mmu.fault_status);
        hosted_exit_code = 1;
        cpu.halted = true;
        return;
    }
    // Save exception state
    cpu.epc = cpu.pc;
    cpu.esr = cpu.sr;

    // Switch to supervisor mode, disable interrupts
    uint32_t old_sr = cpu.sr;
    cpu.sr = (cpu.sr | SR_S) & ~SR_I;
    bank_sp(old_sr, cpu.sr);

    // Read handler address from vector table (physical, MMU bypassed)
    bool bus_fault;
    uint32_t handler = phys_read((uint32_t)(vector * 4), 2, bus_fault);
    cpu.pc = handler;
    pc_written = true;
    cpu.ei_shadow = false;
}

// ═══════════════════════════════════════════════════════════════
// Condition Evaluator
// ═══════════════════════════════════════════════════════════════

static bool eval_cond(int cond) {
    bool n = cpu.sr & SR_N, z = cpu.sr & SR_Z, c = cpu.sr & SR_C, v = cpu.sr & SR_V;
    switch (cond) {
        case 0:  return true;       // AL
        case 1:  return z;          // EQ
        case 2:  return !z;         // NE
        case 3:  return c;          // CS/HS
        case 4:  return !c;         // CC/LO
        case 5:  return n;          // MI
        case 6:  return !n;         // PL
        case 7:  return v;          // VS
        case 8:  return !v;         // VC
        case 9:  return c && !z;    // HI
        case 10: return !c || z;    // LS
        case 11: return n == v;     // GE
        case 12: return n != v;     // LT
        case 13: return !z && (n==v); // GT
        case 14: return z || (n!=v);  // LE
        case 15: return true;       // BL (always, handled specially)
        default: return false;
    }
}

// ═══════════════════════════════════════════════════════════════
// ALU with Flag Generation
// ═══════════════════════════════════════════════════════════════

struct AluResult { uint32_t val; bool n, z, c, v; };

static AluResult alu_exec(int op, uint32_t a, uint32_t b, bool carry_in) {
    AluResult r = {};
    bool sub_mode = (op == 1 || op == 12); // SUB or SBC
    uint32_t b_eff = sub_mode ? ~b : b;

    uint32_t cin;
    if (op == 11 || op == 12) cin = carry_in ? 1 : 0;  // ADC/SBC
    else cin = sub_mode ? 1 : 0;

    uint64_t sum64 = (uint64_t)a + b_eff + cin;

    switch (op) {
        case 0: case 11: // ADD, ADC
            r.val = (uint32_t)sum64; r.c = (sum64 >> 32) & 1;
            r.v = (a>>31 == b_eff>>31) && (r.val>>31 != a>>31); break;
        case 1: case 12: // SUB, SBC
            r.val = (uint32_t)sum64; r.c = (sum64 >> 32) & 1;
            r.v = (a>>31 == b_eff>>31) && (r.val>>31 != a>>31); break;
        case 2: r.val = a & b; break;   // AND
        case 3: r.val = a | b; break;   // OR
        case 4: r.val = a ^ b; break;   // XOR
        case 5: { // SHL
            uint32_t sh = b & 31;
            r.val = a << sh;
            r.c = sh ? (a >> (32 - sh)) & 1 : 0;
            break;
        }
        case 6: { // SHR
            uint32_t sh = b & 31;
            r.val = a >> sh;
            r.c = sh ? (a >> (sh - 1)) & 1 : 0;
            break;
        }
        case 7: { // SAR
            uint32_t sh = b & 31;
            r.val = (uint32_t)((int32_t)a >> sh);
            r.c = sh ? (a >> (sh - 1)) & 1 : 0;
            break;
        }
        case 8: r.val = a; break;      // PASS_A (MOV)
        case 9: r.val = b; break;      // PASS_B (used internally)
        case 10: r.val = ~b; break;    // NOT
        default: r.val = 0; break;
    }
    r.n = (r.val >> 31) & 1;
    r.z = (r.val == 0);
    return r;
}

static void update_flags(const AluResult& r) {
    cpu.sr = (cpu.sr & ~SR_FLAGS) |
             (r.n ? SR_N : 0) | (r.z ? SR_Z : 0) |
             (r.c ? SR_C : 0) | (r.v ? SR_V : 0);
}

// ═══════════════════════════════════════════════════════════════
// Memory Load/Store (virtual → physical → device)
// ═══════════════════════════════════════════════════════════════

// Load from virtual address. Returns value, may take exception.
// size: 0=byte, 1=half, 2=word. sign_ext for LDBS/LDHS.
static uint32_t mem_load(uint32_t vaddr, int size, bool sign_ext, bool& took_exception) {
    took_exception = false;
    uint32_t paddr;
    int fault = mmu_translate(vaddr, 1 /*read*/, size, false, paddr);
    if (fault) { exception_entry(fault); took_exception = true; return 0; }

    bool bus_fault;
    uint32_t raw = phys_read(paddr, size, bus_fault);
    if (bus_fault) {
        mmu.fault_addr = vaddr;
        mmu.fault_status = (uint32_t)((!(cpu.sr&SR_S))<<11) | (1u<<8) | 0x04;
        exception_entry(VEC_BUS_FAULT);
        took_exception = true;
        return 0;
    }

    // Byte/half extraction + sign extension
    if (size == 0) {
        raw &= 0xFF;
        if (sign_ext && (raw & 0x80)) raw |= 0xFFFFFF00;
    } else if (size == 1) {
        raw &= 0xFFFF;
        if (sign_ext && (raw & 0x8000)) raw |= 0xFFFF0000;
    }
    return raw;
}

// Store to virtual address. May take exception.
static void mem_store(uint32_t vaddr, uint32_t data, int size, bool& took_exception) {
    took_exception = false;
    uint32_t paddr;
    int fault = mmu_translate(vaddr, 2 /*write*/, size, false, paddr);
    if (fault) { exception_entry(fault); took_exception = true; return; }

    bool bus_fault;
    phys_write(paddr, data, size, bus_fault);
    if (bus_fault) {
        mmu.fault_addr = vaddr;
        mmu.fault_status = (uint32_t)((!(cpu.sr&SR_S))<<11) | (2u<<8) | 0x04;
        exception_entry(VEC_BUS_FAULT);
        took_exception = true;
    }
}

// ═══════════════════════════════════════════════════════════════
// Instruction Fetch
// ═══════════════════════════════════════════════════════════════

static uint32_t insn_fetch(bool& took_exception) {
    took_exception = false;
    uint32_t paddr;
    int fault = mmu_translate(cpu.pc, 4 /*exec*/, 2 /*word*/, false, paddr);
    if (fault) { exception_entry(fault); took_exception = true; return 0; }

    bool bus_fault;
    uint32_t insn = phys_read(paddr, 2, bus_fault);
    if (bus_fault) {
        mmu.fault_addr = cpu.pc;
        mmu.fault_status = (uint32_t)((!(cpu.sr&SR_S))<<11) | (4u<<8) | 0x04;
        exception_entry(VEC_BUS_FAULT);
        took_exception = true;
        return 0;
    }
    return insn;
}

// ═══════════════════════════════════════════════════════════════
// Hosted Mode Syscalls
// ═══════════════════════════════════════════════════════════════

static bool handle_hosted_syscall() {
    uint32_t nr = reg_read(11); // R11 = syscall number
    if (nr == 1) { // SYS_exit
        hosted_exit_code = (int)reg_read(1); // R1 = status
        cpu.halted = true;
        return true;
    } else if (nr == 4) { // SYS_write
        uint32_t fd = reg_read(1);  // R1 = fd
        uint32_t buf = reg_read(2); // R2 = buf
        uint32_t len = reg_read(3); // R3 = len
        if (fd == 1 || fd == 2) {
            for (uint32_t i = 0; i < len; i++) {
                bool took_exc;
                uint8_t c = (uint8_t)mem_load(buf + i, 0, false, took_exc);
                if (took_exc) break;
                if (write((int)fd, &c, 1) != 1) { /* ignore */ }
            }
            reg_write(1, len); // Success: return len
        } else {
            reg_write(1, (uint32_t)-1); // Error
        }
        return true;
    }
    return false; // Not a hosted syscall we handle
}

// ═══════════════════════════════════════════════════════════════
// Instruction Execute
// ═══════════════════════════════════════════════════════════════

static void execute_one() {
    // Step timer (one instruction = one prescaler step)
    timer.step();

    // Check for pending interrupts before fetching.
    // Timer has priority over external (UART) IRQ.
    if ((cpu.sr & SR_I) && !cpu.ei_shadow) {
        if (timer.irq()) {
            exception_entry(VEC_TIMER);
            return;
        }
        // External IRQ: shared wired-OR of UART + SPI
        bool spi_irq = (spi.xfer_done && (spi.irq_enable & 1))
                     || (spi.rx_thresh() && (spi.irq_enable & 2))
                     || (spi.tx_thresh() && (spi.irq_enable & 4));
        if (uart.irq() || spi_irq) {
            exception_entry(VEC_EXT_IRQ);
            return;
        }
    }

    // DEBUG: catch "jumped to NULL" bugs.  User-mode PC=0 is never
    // legitimate under normal operation — no code lives on page 0
    // (VM_MIN_ADDRESS is 0x1000, null page is an unmapped guard).
    // Gated behind +trap-pc0 because OS-level tests can legitimately
    // probe memory protection by jumping to address 0 and recovering
    // from the SIGSEGV; firing the trap would mask that behavior.
    // `prev_pc` (set at the bottom of this function) gives the
    // address of the instruction that set PC=0.
    static uint32_t prev_pc = 0;
    if (trap_user_pc_zero && cpu.pc == 0 && !(cpu.sr & SR_S)) {
        fprintf(stderr,
            "\n[iss] user PC=0 (null-pointer jump) from prev PC=0x%08x:\n",
            prev_pc);
        fprintf(stderr, "  PC=%08x LR=%08x SP=%08x SR=%08x\n",
                cpu.pc, cpu.r[13], cpu.r[14], cpu.sr);
        for (int r = 1; r <= 12; r++)
            fprintf(stderr, "  R%d=%08x%s", r, cpu.r[r],
                    (r % 4 == 0) ? "\n" : "");
        fprintf(stderr, "\n  insn_count=%llu\n",
                (unsigned long long)cpu.insn_count);
        fflush(stderr);
        std::abort();
    }
    prev_pc = cpu.pc;

    bool took_exception;
    uint32_t insn = insn_fetch(took_exception);
    if (took_exception) return;

    // Clear ei_shadow after one instruction post-EI
    bool had_ei_shadow = cpu.ei_shadow;
    cpu.ei_shadow = false;

    pc_written = false;
    int fmt = (insn >> 30) & 3;

    switch (fmt) {
    // ── Format R: Register operations ────────────────────────
    case 0: {
        int op = (insn >> 25) & 0x1F;
        int rd = (insn >> 21) & 0xF;
        int rs = (insn >> 17) & 0xF;
        bool f_bit = (insn >> 16) & 1;
        int sys_dev = (insn >> 12) & 0xF;
        int sys_reg = (insn >> 8) & 0xF;
        int spr_num = (insn >> 12) & 0xF;

        if (op <= 11) {
            // ALU: ADD SUB AND OR XOR SHL SHR SAR MOV NOT ADC SBC
            // ISA opcodes 9-11 don't map 1:1 to ALU ops:
            //   ISA 9=NOT→ALU 10, ISA 10=ADC→ALU 11, ISA 11=SBC→ALU 12
            static const int isa_to_alu[] = {0,1,2,3,4,5,6,7,8,10,11,12};
            int alu_op = isa_to_alu[op];

            uint32_t a, b;
            if (op == 8) { // MOV: Rd = Rs (a=Rs)
                a = reg_read(rs); b = 0;
            } else if (op == 9) { // NOT: Rd = ~Rs (b=Rs)
                a = 0; b = reg_read(rs);
            } else { // Two-operand: a=Rd, b=Rs
                a = reg_read(rd); b = reg_read(rs);
            }
            bool cin = (cpu.sr & SR_C) != 0;
            AluResult r = alu_exec(alu_op, a, b, cin);

            // MOV doesn't update flags; all others do
            if (op != 8) update_flags(r);
            // F-bit suppresses register write (CMP = SUB+F, TEST = AND+F)
            if (!f_bit) reg_write(rd, r.val);

        } else if (op <= 17) {
            // MUL/MULU/DIV/DIVU/MOD/MODU — trap as illegal
            exception_entry(VEC_ILLEGAL);

        } else if (op <= 22) {
            // Reserved gap — illegal
            exception_entry(VEC_ILLEGAL);

        } else {
            // System instructions (op 23-31)
            bool priv_required = true;
            if (op == 25 || op == 26) priv_required = false; // SYSCALL, BREAK

            if (priv_required && !(cpu.sr & SR_S)) {
                exception_entry(VEC_PRIV);
                break;
            }

            switch (op) {
            case 23: // WRSYS
                sysreg_write(sys_dev, sys_reg, reg_read(rd));
                break;
            case 24: // RDSYS
                reg_write(rd, sysreg_read(sys_dev, sys_reg));
                break;
            case 25: // SYSCALL
                if (hosted_mode && handle_hosted_syscall()) {
                    // Handled natively by simulator
                } else {
                    exception_entry(VEC_SYSCALL);
                }
                break;
            case 26: // BREAK
                /*
                 * Hardware: BREAK enters except_entry → VEC_BREAK.
                 * RTL testbench samples o_halted at dispatch and exits
                 * the sim before the trap completes — useful for test
                 * programs that end with BREAK as a "done" sentinel.
                 *
                 * For interactive kernel runs (DDB), we want the trap
                 * to fire so the kernel handler runs.  +halt-on-break
                 * restores the testbench-style exit.
                 */
                if (halt_on_break) {
                    cpu.halted = true;
                } else {
                    exception_entry(VEC_BREAK);
                }
                break;
            case 27: { // ERET
                uint32_t old_sr = cpu.sr;
                cpu.sr = cpu.esr;
                bank_sp(old_sr, cpu.sr);
                cpu.pc = cpu.epc;
                pc_written = true;
                break;
            }
            case 28: // EI
                cpu.sr |= SR_I;
                cpu.ei_shadow = true;
                break;
            case 29: // DI
                cpu.sr &= ~SR_I;
                break;
            case 30: // WRSPR
                switch (spr_num) {
                    case SPR_ESR: cpu.esr = reg_read(rd); break;
                    case SPR_EPC: cpu.epc = reg_read(rd); break;
                    case SPR_USP: cpu.usp = reg_read(rd); break;
                    case SPR_SR: {
                        uint32_t old_sr = cpu.sr;
                        cpu.sr = reg_read(rd);
                        bank_sp(old_sr, cpu.sr);
                        break;
                    }
                    default:
                        if (spr_num >= SPR_SCR0 && spr_num < SPR_SCR0 + N_SCR) {
                            cpu.scr[spr_num - SPR_SCR0] = reg_read(rd);
                        }
                        break;
                }
                break;
            case 31: // RDSPR
                switch (spr_num) {
                    case SPR_ESR: reg_write(rd, cpu.esr); break;
                    case SPR_EPC: reg_write(rd, cpu.epc); break;
                    case SPR_USP: reg_write(rd, cpu.usp); break;
                    case SPR_SR:  reg_write(rd, cpu.sr); break;
                    default:
                        if (spr_num >= SPR_SCR0 && spr_num < SPR_SCR0 + N_SCR) {
                            reg_write(rd, cpu.scr[spr_num - SPR_SCR0]);
                        }
                        break;
                }
                break;
            }
        }
        break;
    }

    // ── Format L: Immediate operations ───────────────────────
    case 1: {
        int op = (insn >> 26) & 0xF;
        int rd = (insn >> 22) & 0xF;
        uint32_t imm16 = insn & 0xFFFF;
        uint32_t rd_val = reg_read(rd);
        bool cin = (cpu.sr & SR_C) != 0;

        switch (op) {
        case 0: // LLI: Rd = zero_extend(imm16)
            reg_write(rd, imm16);
            break;
        case 1: // LLIS: Rd = sign_extend(imm16)
            reg_write(rd, (imm16 & 0x8000) ? (imm16 | 0xFFFF0000) : imm16);
            break;
        case 2: // LUI: Rd = Rd | (imm16 << 16)
            reg_write(rd, rd_val | (imm16 << 16));
            break;
        case 3: { // ADD #imm
            AluResult r = alu_exec(0, rd_val, imm16, cin);
            update_flags(r); reg_write(rd, r.val); break;
        }
        case 4: { // SUB #imm
            AluResult r = alu_exec(1, rd_val, imm16, cin);
            update_flags(r); reg_write(rd, r.val); break;
        }
        case 5: { // CMP #imm (flags only)
            AluResult r = alu_exec(1, rd_val, imm16, cin);
            update_flags(r); break;
        }
        case 6: { // AND #imm
            AluResult r = alu_exec(2, rd_val, imm16, cin);
            update_flags(r); reg_write(rd, r.val); break;
        }
        case 7: { // TEST #imm (flags only)
            AluResult r = alu_exec(2, rd_val, imm16, cin);
            update_flags(r); break;
        }
        case 8: { // SHL #imm
            AluResult r = alu_exec(5, rd_val, imm16, cin);
            update_flags(r); reg_write(rd, r.val); break;
        }
        case 9: { // SHR #imm
            AluResult r = alu_exec(6, rd_val, imm16, cin);
            update_flags(r); reg_write(rd, r.val); break;
        }
        case 10: { // SAR #imm
            AluResult r = alu_exec(7, rd_val, imm16, cin);
            update_flags(r); reg_write(rd, r.val); break;
        }
        case 11: // JMP Rd (PC = Rd)
            cpu.pc = rd_val;
            pc_written = true;
            break;
        case 12: // JALR Rd (R13 = PC+4, PC = Rd)
            reg_write(13, cpu.pc + 4);
            cpu.pc = rd_val;
            pc_written = true;
            break;
        default:
            exception_entry(VEC_ILLEGAL);
            break;
        }
        break;
    }

    // ── Format M: Memory load/store ──────────────────────────
    case 2: {
        bool is_load = (insn >> 29) & 1;
        int  sz      = (insn >> 27) & 3;   // 00=byte, 01=half, 10=word
        bool se      = (insn >> 26) & 1;   // sign-extend on load
        int  rd      = (insn >> 22) & 0xF;
        int  rb      = (insn >> 18) & 0xF;
        int16_t offset = (int16_t)(uint16_t)((insn >> 2) & 0xFFFF);
        uint32_t ea = reg_read(rb) + (int32_t)offset;

        bool exc;
        if (is_load) {
            uint32_t val = mem_load(ea, sz, se, exc);
            if (!exc) reg_write(rd, val);
        } else {
            mem_store(ea, reg_read(rd), sz, exc);
        }
        break;
    }

    // ── Format B: Branch ─────────────────────────────────────
    case 3: {
        int cond = (insn >> 26) & 0xF;
        int32_t offset22 = (int32_t)((insn >> 4) & 0x3FFFFF);
        // Sign-extend 22-bit offset
        if (offset22 & 0x200000) offset22 |= (int32_t)0xFFC00000;
        uint32_t target = cpu.pc + (uint32_t)(offset22 * 4);

        if (cond == 15) {
            // BL: save return address, then branch (always taken)
            reg_write(13, cpu.pc + 4);
            cpu.pc = target;
            pc_written = true;
        } else if (eval_cond(cond)) {
            cpu.pc = target;
            pc_written = true;
        }
        break;
    }
    } // switch(fmt)

    // Advance PC if instruction didn't modify it
    if (!pc_written && !cpu.halted) cpu.pc += 4;

    // Restore ei_shadow if it was set at entry (don't clear prematurely)
    if (had_ei_shadow) cpu.ei_shadow = false;

    cpu.insn_count++;
}

// ═══════════════════════════════════════════════════════════════
// Trace Output
// ═══════════════════════════════════════════════════════════════

static void trace_insn() {
    if (!trace_fp) return;
    uint32_t sr = cpu.sr;
    fprintf(trace_fp, "PC=%08x SR=%08x [%c%c%c%c]",
            cpu.pc, sr,
            (sr & SR_N) ? 'N' : '-',
            (sr & SR_Z) ? 'Z' : '-',
            (sr & SR_C) ? 'C' : '-',
            (sr & SR_V) ? 'V' : '-');
    for (int r = 1; r <= 14; r++)
        fprintf(trace_fp, " R%d=%08x", r, cpu.r[r]);
    fprintf(trace_fp, "\n");
}

// ═══════════════════════════════════════════════════════════════
// Program Loading
// ═══════════════════════════════════════════════════════════════

static bool load_hex(const char* path, uint8_t* mem, size_t max_size, uint32_t offset = 0) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open '%s'\n", path); return false; }
    char line[256];
    size_t addr = offset;
    while (fgets(line, sizeof(line), f) && addr < max_size) {
        if (line[0] == '/' || line[0] == '\n' || line[0] == '\r') continue;
        uint32_t word;
        if (sscanf(line, "%x", &word) == 1) {
            if (addr + 4 <= max_size) {
                wr32(&mem[addr], word);
                addr += 4;
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[ROM] loaded %zu bytes from '%s'\n", addr, path);
    return true;
}

// ═══════════════════════════════════════════════════════════════
// UART RX Polling (stdin → UART)
// ═══════════════════════════════════════════════════════════════

// Escape prefix state for +raw mode (Ctrl-A is the escape character).
static bool esc_pending = false;

const char * const register_names[] = {
    "R0",
    "R1",
    "R2",
    "R3",
    "R4",
    "R5",
    "R6",
    "R7",
    "R8",
    "R9",
    "R10",
    "R11",
    "R12",
    "LR",
    "SP",
    "PC"
};

static void print_sr_flag(uint32_t mask, char flag) {
    flag = (cpu.sr & mask) ? flag : '-';
    fprintf(stderr, "%c", flag);
}

static void print_cpu_state() {
    fprintf(stderr, "\r\n----- CPU STATE -----\r\n");
    for (int i = 0; i < 16; i++) {
        if (i > 0) {
            if ((i % 6) == 0) {
                fprintf(stderr, "\r\n");
            } else {
                fprintf(stderr, " ");
            }
        }
        uint32_t regval = i == 15 ? cpu.pc : cpu.r[i];
        fprintf(stderr, "%3s=%08X", register_names[i], regval);
    }
    fprintf(stderr, "\r\n");
    fprintf(stderr, "SR=%08x (", cpu.sr);
    print_sr_flag(SR_N, 'N');
    print_sr_flag(SR_Z, 'Z');
    print_sr_flag(SR_C, 'C');
    print_sr_flag(SR_V, 'V');
    print_sr_flag(SR_I, 'I');
    print_sr_flag(SR_S, 'S');
    fprintf(stderr, ")\r\n");
}

static void print_cmd_help() {
    fprintf(stderr, "\r\n");
    fprintf(stderr, "Available commands:\r\n\r\n");
    fprintf(stderr, "Ctrl-A H: This help\r\n");
    fprintf(stderr, "Ctrl-A X: Exit simulator\r\n");
    fprintf(stderr, "Ctrl-A C: Dump CPU state\r\n");
    fprintf(stderr, "Ctrl-A B: Send serial BREAK (triggers DDB)\r\n");
    fprintf(stderr, "\r\n");
}

static bool handle_escape(char c) {
    switch (c) {
        case '\01':
            return false;
        case 'x':
        case 'X':
            running = false;
            break;
        case 'h':
        case 'H':
            print_cmd_help();
            break;
        case 'c':
        case 'C':
            print_cpu_state();
            break;
        case 'b':
        case 'B':
            /*
             * Deliver a serial-line BREAK condition.  Sets LSR.BI and
             * a null byte in RBR; the kernel's com(4) ISR sees BI when
             * reading LSR, calls cn_check_magic(CNC_BREAK, ...), which
             * (with the default magic) invokes Debugger() → DDB.
             */
            uart.rbr = 0x00;
            uart.rx_ready = true;
            uart.rx_break = true;
            break;
    }
    return true;
}

static void poll_uart_rx() {
    if (uart.rx_ready) return;
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) return;

        if (full_raw) {
            if (esc_pending) {
                esc_pending = false;
                if (handle_escape(c)) return;
                // handle_escape returned false — feed byte to UART
            } else if (c == '\x01') {  // Ctrl-A
                esc_pending = true;
                return;
            }
        }

        uart.rbr = (uint8_t)c;
        uart.rx_ready = true;
    }
}

// ═══════════════════════════════════════════════════════════════
// Reset
// ═══════════════════════════════════════════════════════════════

static void cpu_reset() {
    memset(&cpu, 0, sizeof(cpu));
    cpu.pc = ROM_BASE;          // Boot from ROM
    cpu.sr = SR_S;              // Start in supervisor mode, IRQs disabled
    memset(&uart, 0, sizeof(uart));
    spi.reset();
    timer.reset();
    busctl.reg = 0;
    acfg.reset();
    memset(&mmu, 0, sizeof(mmu));
    memset(&tlb, 0, sizeof(tlb));
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    const char* hex_path = nullptr;
    const char* sd_path = nullptr;
    const char* trace_path = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "+sdcard=", 8) == 0) sd_path = argv[i] + 8;
        else if (strncmp(argv[i], "+trace=", 7) == 0) trace_path = argv[i] + 7;
        else if (strncmp(argv[i], "+max-insn=", 10) == 0) max_insns = strtoull(argv[i] + 10, nullptr, 0);
        else if (strcmp(argv[i], "+raw") == 0) full_raw = true;
        else if (strcmp(argv[i], "+hosted") == 0) hosted_mode = true;
        else if (strcmp(argv[i], "+quiet") == 0) quiet_mode = true;
        else if (strcmp(argv[i], "+trap-pc0") == 0) trap_user_pc_zero = true;
        else if (strcmp(argv[i], "+halt-on-break") == 0) halt_on_break = true;
        else hex_path = argv[i];
    }

    if (!hex_path) {
        fprintf(stderr, "Usage: penumbra-iss [program.hex] [+sdcard=path] [+trace=path] [+raw] [+hosted] [+quiet] [+max-insn=N] [+trap-pc0] [+halt-on-break]\n");
        return 1;
    }

    // Initialize
    memset(ram, 0, sizeof(ram));
    memset(rom, 0, sizeof(rom));
    cpu_reset();

    if (hosted_mode) {
        if (!load_hex(hex_path, ram, RAM_SIZE, 0x1000)) return 1;
        cpu.pc = 0x1000;
    } else {
        if (!load_hex(hex_path, rom, ROM_SIZE)) return 1;
    }

    SdCardSim sd(sd_path);
    sd_card = sd.is_present() ? &sd : nullptr;
    if (sd_card) fprintf(stderr, "[SD] card emulation active\n");

    if (trace_path) {
        trace_fp = fopen(trace_path, "w");
        if (trace_fp) fprintf(stderr, "[TRACE] writing to '%s'\n", trace_path);
        else fprintf(stderr, "[TRACE] cannot open '%s'\n", trace_path);
    }

    if (!full_raw)
        signal(SIGINT, sigint_handler);
    if (!hosted_mode)
        raw_mode();

    if (!quiet_mode) {
        if (full_raw)
            fprintf(stderr, "── Penumbra ISS (Ctrl-A X to exit, Ctrl-A H for help) ──\n\n");
        else
            fprintf(stderr, "── Penumbra ISS (Ctrl-C to exit) ──\n\n");
    }

    // Main loop
    while (running && !cpu.halted) {
        if (max_insns > 0 && cpu.insn_count >= max_insns) {
            fprintf(stderr, "\n[ERROR] Instruction limit reached (%llu)\n", (unsigned long long)max_insns);
            hosted_exit_code = 124; // Standard timeout exit code
            cpu.halted = true;
            break;
        }
        trace_insn();
        execute_one();

        // UART TX busy countdown (models baud delay)
        if (uart.tx_busy_count > 0 && --uart.tx_busy_count == 0)
            uart.thre_int = true;

        // Poll stdin periodically for UART RX
        if ((cpu.insn_count & 0xFF) == 0) poll_uart_rx();
    }

    // Restore terminal before printing exit summary so \n works normally
    restore_term();

    if (!quiet_mode) {
        if (cpu.halted) {
            fprintf(stderr, "\n[BREAK after %lu instructions, PC=0x%08X]\n",
                    (unsigned long)cpu.insn_count, cpu.pc);
            // Dump watched memory values from raw RAM array
            for (int wi = 0; wi < dbg.watch_count; wi++) {
                uint32_t wpa = dbg.watch_pa[wi];
                if (wpa < RAM_SIZE - 3) {
                    uint32_t val = rd32(&ram[wpa & ~3u]);
                    fprintf(stderr, "[W%d] PA=0x%08X raw RAM value=0x%08X\n",
                            wi, wpa, val);
                }
            }
        } else {
            fprintf(stderr, "\n[Interrupted after %lu instructions, PC=0x%08X]\n",
                    (unsigned long)cpu.insn_count, cpu.pc);
        }
    }
    if (!quiet_mode) {
        fprintf(stderr, "SR=%08X", cpu.sr);
        for (int r = 1; r <= 14; r++)
            fprintf(stderr, " R%d=%08x", r, cpu.r[r]);
        fprintf(stderr, "\n");
    }

    if (trace_fp) { fclose(trace_fp); fprintf(stderr, "[TRACE] done\n"); }
    return hosted_mode ? hosted_exit_code : 0;
}
