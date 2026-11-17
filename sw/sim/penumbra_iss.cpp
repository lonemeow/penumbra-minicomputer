// Penumbra Instruction Set Simulator (ISS)
//
// Fast instruction-level simulator for software development.
// No Verilator dependency — compiles with just g++ -O2.
//
// Matches the RTL machine_sim memory map and device set:
//   0x00000000 – ...         RAM (32 MB, the ULX3S's; +ram= sizes it)
//   0xFE000000 – 0xFE00001F  Autoconfig space (when cfg_en)
//   0xFF000000 – 0xFF000FFF  UART (16450-compatible)
//   0xFFFF0000 – 0xFFFFFFFF  Boot ROM (64 KB)
//   SPI/SD, USBHC, then the display adapter at dynamic bases
//   (assigned via autoconfig, chained in the hardware order)
//
// Trace output format (compatible with RTL tb_interactive):
//   PC=XXXXXXXX SR=XXXXXXXX [SVNZCV] R1=... R2=... ... R14=...
//
// Usage: penumbra-iss [program.hex] [+ram=size] [+sdcard=path] [+trace=path]
//                     [+raw] [+trap-pc0] [+halt-on-break] [+core=generation]
//                     [+eth=backend]
//
// +ram= takes a byte count with an optional K, M or G suffix (powers of
// 1024), e.g. +ram=512M for the Nexys Video's DDR3.
//
// +core= names the hardware generation the ISS presents, spelled as
// `make CORE=` spells it (penumbra_cores.h): its CPU name and the
// geometry its caches and its MMU report.  The default is penumbra1.
//
// +mmu=VER[:SETSxWAYS] presents MMU interface version VER, and the main
// TLB SETSxWAYS when given, in place of the generation's, so software
// can meet an MMU no generation builds; MMU_CONFIG reports it.
//
// +eth=backend[,mdio][,rx=N][,tx=N][,speed=MBIT] attaches an Ethernet
// adapter (CLASS_ETHERNET) at the end of the autoconfig chain, with N
// slots in each ring; the machine has none without it.  The loop
// backend hands every frame the device sends to the device's own
// receive side, and tap[:name] exchanges frames with a Linux TAP device
// (tap0 unless named), which sw/tools/iss-net.sh sets up without root.
// The device manages its own link unless mdio gives it a PHY, which
// software then has to manage.
//
// +cachemodel[=list] runs every access through shadow cache hierarchies
// (cache_model.h), and +tlbmodel[=list] every main-TLB translation through
// shadow TLBs (tlb_model.h); both report what they hit and missed, per
// measurement window: +model_start= / +model_stop= PCs, SIGUSR1,
// +model_report=path.  +sbmodel[=list] runs every store through shadow
// store buffers on an estimated clock (sb_model.h), with the fills of the
// first +cachemodel hierarchy and +sbmodel_cpi= cycles an instruction,
// and reports the waits they cause in the same windows.  +bpmodel[=list]
// runs every control transfer through shadow branch predictors
// (bp_model.h) and reports, in the same windows, where each would have
// redirected fetch.  +pairmodel[=list] runs every instruction through
// shadow two-slot issue stages (pair_model.h) and reports the issue
// cycles each would take and what kept instructions from pairing.
// +loadusemodel[=list] runs every instruction through shadow
// single-issue pipelines (loaduse_model.h) and reports the cycles
// instructions wait for what a load, or an op behind one, writes.
//
// +raw enables full raw TTY mode: all control characters (Ctrl-C,
// Ctrl-Z, etc.) pass through to the simulated UART for job control
// inside the guest OS.  Use Ctrl-A as escape prefix:
//   Ctrl-A X     — exit simulator
//   Ctrl-A C     — dump CPU state
//   Ctrl-A B     — send serial BREAK (triggers DDB if enabled)
//   Ctrl-A U     — toggle USB device attach (hotplug)
//   Ctrl-A D     — dump the display screen as text
//   Ctrl-A H     — help
//   Ctrl-A Ctrl-A — send literal Ctrl-A

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <algorithm>
#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#endif
#include <map>
#include <string>
#include <vector>

// Byte-level USB device responder shared with the RTL testbenches —
// the one device model both simulators enumerate against.
#include "../../hw/sim/usb_device_sim.h"
#include "../../hw/sim/usb_msc_sim.h"

#include "penumbra_cores.h"
#include "cache_model.h"
#include "tlb_model.h"
#include "sb_model.h"
#include "bp_model.h"
#include "pair_model.h"
#include "loaduse_model.h"

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
       VEC_EXT_IRQ=9, VEC_ARITH=10 };

// Sysreg device IDs
enum { SYSDEV_MMU=0, SYSDEV_CPU=1, SYSDEV_L1_DCACHE=2, SYSDEV_L1_ICACHE=3, SYSDEV_BUS=4,
       SYSDEV_TIMER=7, SYSDEV_MACH=8, SYSDEV_L2_CACHE=9, SYSDEV_DEBUG=15 };

// Cache device registers (the unified map every cache shares)
enum { CACHE_REG_INFO=0, CACHE_REG_CTRL=1, CACHE_REG_INVAL_ALL=2 };

// Debug watchpoints — halt on physical memory write to watched addresses
static constexpr int DBG_MAX_WATCH = 16;
static struct {
    uint32_t watch_pa[DBG_MAX_WATCH];
    int      watch_count;
    uint32_t watch_val;    // Only trigger when this value is written (0xFFFFFFFF = any)
} dbg;

// MMU sysreg addresses
enum { MMU_CR=0, MMU_FADDR=1, MMU_FSTAT=2, MMU_TLB_VPN=3, MMU_TLB_PTE=4, MMU_TLB_IDX=5,
       MMU_INVAL_VA=6, MMU_INVAL_ASID=7, MMU_INVAL_ALL=8, MMU_CFG=15 };

// The MMU this model presents (doc/system/mmu.md): the interface version
// and the main TLB's geometry, the +core= generation's unless +mmu= names
// others, beside 8 pinned slots, 4 KB pages and an 8-bit ASID.  Software
// selects its TLB miss handler from the MMU_CONFIG value that reports it.
static constexpr int      PTLB_ENTRIES  = 8;
static constexpr uint32_t MMU_PAGE_LOG2 = 12;
static constexpr uint32_t MMU_ASID_BITS = 8;
static uint32_t    mmu_version;
static TlbGeometry mmu_geometry;

static uint32_t mmu_config_value() {
    return mmu_version | PTLB_ENTRIES << 4 | __builtin_ctz(mmu_geometry.sets) << 10 |
           mmu_geometry.ways << 15 | MMU_PAGE_LOG2 << 20 | MMU_ASID_BITS << 25;
}

static constexpr uint32_t PAGE_BYTES = 1u << MMU_PAGE_LOG2;

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

// Memory sizes. RAM is the ULX3S's 32 MB unless +ram= sizes it.
static size_t ram_size = 32u * 1024 * 1024;
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

static uint8_t* ram;                // ram_size bytes, zeroed (allocated in main)
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

// --- Autoconfig device chain ---
// The machine_sim chain order: SPI/SD first, then the USB host
// controller. The Ethernet adapter ends the chain when +eth= attaches
// one. The config window shows the first unconfigured device;
// once every device is configured, config-space reads fall through to
// a bus fault — "no more devices" to the autoconfig loop.
struct AcfgSlot {
    uint32_t dev_class;     // ACFG_CLASS code
    uint32_t size;          // window size (power of two)
    uint32_t name0;         // ACFG_NAME0, packed LE
    uint32_t name1;         // ACFG_NAME1, packed LE (NAME2-3 read zero)
    bool     configured;
    uint32_t base_addr;
    bool     absent = false;    // not in the chain; the config window skips it

    bool dev_sel(uint32_t addr) const {
        return configured && ((addr & ~(size - 1)) == base_addr);
    }
};

enum { ACFG_SLOT_SPI = 0, ACFG_SLOT_USB = 1, ACFG_SLOT_DISPLAY = 2,
       ACFG_SLOT_AUDIO = 3, ACFG_SLOT_ETH = 4, ACFG_SLOT_COUNT = 5 };

static struct {
    AcfgSlot slot[ACFG_SLOT_COUNT] = {
        { 4 /*CLASS_SD*/,      4096, 0x00004453 /*"SD\0\0"*/, 0, false, 0 },
        { 8 /*CLASS_USBHC*/,   4096, 0x00425355 /*"USB\0"*/,  0, false, 0 },
        { 6 /*CLASS_DISPLAY*/, 262144,
          0x50534944 /*"DISP"*/, 0x0059414C /*"LAY\0"*/, false, 0 },
        { 7 /*CLASS_AUDIO*/,   4096, 0x4F445541 /*"AUDO"*/, 0, false, 0 },
        // Sized, and made present, by +eth=.
        { 5 /*CLASS_ETHERNET*/, 0x20000, 0x00485445 /*"ETH\0"*/, 0, false, 0,
          true },
    };

    void reset() {
        for (auto& s : slot) { s.configured = false; s.base_addr = 0; }
    }
    // The slot the config window currently shows, or null past chain end.
    AcfgSlot* cfg_slot(bool cfg_en, uint32_t addr) {
        if (!cfg_en || (addr >> 5) != (ACFG_BASE >> 5)) return nullptr;
        for (auto& s : slot)
            if (!s.absent && !s.configured) return &s;
        return nullptr;
    }
} acfg;

// --- USB host controller (CLASS_USBHC, transaction-level) ---
// Register contract: doc/system/devices/usb-host.md; the executable
// spec is hw/sim/tb_usbhc_dev.cpp — behaviors pinned there hold here.
// The ISS is transaction-level: a transaction launched by XFER_CTRL
// completes within that store (no SIE or line timing), exchanging
// complete packets with the shared UsbDeviceSim. The frame counter
// ticks on an instruction prescale like the timer.
static constexpr int USB_BUF_BYTES = 64;
// 1 ms frame marker at the timer's instruction≈cycle approximation
// (25 MHz clock: TIMER_PRESCALE × 1000).
static constexpr int USB_FRAME_PRESCALE = 25000;

// XFER_STATUS.RESULT codes (penumbra_pkg USBHC_RESULT_*)
enum { USB_RES_ACK=0, USB_RES_NAK=1, USB_RES_STALL=2, USB_RES_TIMEOUT=3,
       USB_RES_ERROR=4, USB_RES_OVERFLOW=5 };

// IRQ_STATUS / IRQ_ENABLE bits
enum { USB_IRQ_XFER=1, USB_IRQ_PORT=2, USB_IRQ_SOF=4 };

static struct {
    // Register state. XFER_STATUS.DONE is irq_status's XFER_DONE bit
    // itself (the RTL mirrors the same sticky W1C flop), so the W1C
    // clears both views at once.
    uint32_t irq_status;    // [0] XFER_DONE [1] PORT_CHANGE [2] SOF (W1C)
    uint32_t irq_enable;
    uint32_t port_ctrl;     // [0] POWER [1] RESET [2] RUN [3] SUSPEND [4] RESUME
    uint32_t token;         // [1:0] PID [10:4] DEVADDR [14:11] ENDPOINT [16] TOGGLE
    uint32_t length;        // XFER_CTRL.LENGTH, held for the transaction
    uint32_t result;        // XFER_STATUS.RESULT
    uint32_t rxlen;         // XFER_STATUS.RXLEN — IN payload bytes, CRC excluded
    bool     rxtoggle;      // XFER_STATUS.RXTOGGLE — toggle of the received DATAx
    uint32_t frame;         // FRAME[10:0]
    uint8_t  buf[USB_BUF_BYTES];   // the DATA window, byte-addressed

    // Port view (PORT_STATUS). The ISS device is full-speed; SPEED
    // reads the programmer encoding (0 none, 2 full), never the
    // UTMI code the RTL seam carries.
    bool connect;
    bool enabled;

    int frame_cnt;          // instruction prescaler toward the next frame

    bool dev_attached;      // a device sits on the port (the ISS default)
    UsbDeviceSim dev;

    bool power() const { return port_ctrl & 1; }
    bool reset_active() const { return (port_ctrl >> 1) & 1; }
    bool run() const { return (port_ctrl >> 2) & 1; }
    bool irq() const { return (irq_status & irq_enable) != 0; }

    void reset() {
        irq_status = irq_enable = 0;
        port_ctrl = token = length = 0;
        result = rxlen = 0; rxtoggle = false;
        frame = 0; frame_cnt = 0;
        memset(buf, 0, sizeof buf);
        connect = enabled = false;
    }
} usb;

// --- Audio output (CLASS_AUDIO, one channel) ---
// Register contract: doc/system/devices/audio.md; the executable spec
// is hw/sim/tb_audio.cpp. The ISS has no timing model, so frames are
// consumed the instant they are written: LEVEL always reports the queue
// empty, and a consumer therefore never blocks and never underruns.
// That is the benign direction to be wrong in — a driver that works
// here may still stall on hardware, but one that stalls here is broken.
// +audiofile=path captures the output as raw 16-bit stereo at
// AUDIO_RATE, which makes a driver's output audible rather than
// inferred.  INC is applied on the way, so a consumer pushing a slower
// rate is captured at the rate it will be heard at rather than at the
// rate it wrote.
//
// +audio_status= presents a device that is not ready, so a driver's
// readiness handling runs without a broken board: busy:N reads BUSY for
// the first N reads of STATUS and then comes up, as a bring-up that
// finishes; busy holds BUSY for good, as one that breaks the class's
// 100 ms bound; fault presents FAULT from reset, and fault:CODE with
// that FAULT_CODE.  Until the device is up it consumes nothing, so
// frames queue as they would on hardware, and play the moment it comes
// up.
static constexpr uint32_t AUDIO_DEPTH     = 1024;
static constexpr uint32_t AUDIO_CHANNELS = 4;
static constexpr uint32_t AUDIO_RATE     = 48000;
static constexpr uint32_t AUDIO_INC_UNITY = 0x00010000u;
// Version 1; PITCH, VOLUME and STATUS implemented, AUDIO_CHANNELS channels.
static constexpr uint32_t AUDIO_CAP =
    1u | (1u << 8) | (1u << 9) | (1u << 13) | (AUDIO_CHANNELS << 16);
static constexpr uint32_t AUDIO_ST_BUSY = 1u << 0, AUDIO_ST_FAULT = 1u << 1;

static struct {
    uint32_t ctrl;          // [0] ENABLE, resets 0
    uint32_t chan_en;
    uint32_t irq_enable;
    uint32_t irq_status;
    uint32_t underrun;
    uint32_t master_vol;
    uint32_t wm[AUDIO_CHANNELS];
    uint32_t inc[AUDIO_CHANNELS];
    uint32_t vol[AUDIO_CHANNELS];
    uint32_t frac[AUDIO_CHANNELS];   // phase within the source frame, 16.16
    FILE*    capture = nullptr;

    // Readiness, as +audio_status set it up and as it stands now.
    // busy_reads counts the STATUS reads left before a finite bring-up
    // ends; held[c] is what channel c queued while the device was not up.
    enum class Stage { ready, busy_for, busy_stuck, fault };
    Stage    stage_cfg = Stage::ready, stage = Stage::ready;
    uint32_t busy_reads_cfg = 0, busy_reads = 0;
    uint32_t fault_code = 0;
    std::vector<uint32_t> held[AUDIO_CHANNELS];

    void reset() {
        ctrl = chan_en = irq_enable = irq_status = underrun = 0;
        master_vol = 0xFFFFFFFFu;
        for (uint32_t c = 0; c < AUDIO_CHANNELS; c++) {
            wm[c]   = 0;
            inc[c]  = AUDIO_INC_UNITY;
            vol[c]  = 0xFFFFFFFFu;
            frac[c] = 0;
            held[c].clear();
        }
        stage      = stage_cfg;
        busy_reads = busy_reads_cfg;
    }

    // +audio_status=busy, busy:N (N >= 1), fault or fault:CODE (0-255).
    // False on anything else, which the caller refuses.
    bool set_stage(const char* spec) {
        if (strcmp(spec, "busy") == 0) {
            stage_cfg = Stage::busy_stuck;
        } else if (strcmp(spec, "fault") == 0) {
            stage_cfg = Stage::fault;
        } else if (strncmp(spec, "fault:", 6) == 0) {
            char* end;
            unsigned long code = strtoul(spec + 6, &end, 0);
            if (spec[6] == '\0' || *end != '\0' || code > 0xFF)
                return false;
            stage_cfg = Stage::fault;
            fault_code = uint32_t(code);
        } else if (strncmp(spec, "busy:", 5) == 0) {
            char* end;
            unsigned long n = strtoul(spec + 5, &end, 10);
            if (spec[5] == '\0' || *end != '\0' || n == 0 || n > UINT32_MAX)
                return false;
            stage_cfg = Stage::busy_for;
            busy_reads_cfg = uint32_t(n);
        } else {
            return false;
        }
        stage = stage_cfg;
        busy_reads = busy_reads_cfg;
        return true;
    }

    uint32_t status() const {
        switch (stage) {
            case Stage::busy_for:
            case Stage::busy_stuck: return AUDIO_ST_BUSY;
            case Stage::fault:      return AUDIO_ST_FAULT | (fault_code << 8);
            default:                return 0;
        }
    }

    uint32_t level(uint32_t c) const {
        return AUDIO_DEPTH - uint32_t(held[c].size());
    }

    // A channel's watermark flag, sticky while its free space has reached
    // an armed watermark.
    void update_watermarks() {
        for (uint32_t i = 0; i < AUDIO_CHANNELS; i++)
            if (wm[i] != 0 && level(i) >= wm[i]) irq_status |= (1u << i);
    }

    // The device consumes INC source frames per output frame, so one
    // source frame covers every output frame whose phase falls inside
    // it, held across them as the device holds it.
    //
    // Only channel 0 is captured: this models the registers rather than
    // the mix, so there is nothing to combine the others into, and
    // channel 0 is what a single-stream consumer drives.
    void play(uint32_t c, uint32_t data) {
        const uint8_t b[4] = { uint8_t(data), uint8_t(data >> 8),
                               uint8_t(data >> 16), uint8_t(data >> 24) };
        // A zero INC would advance the phase not at all; the class floor
        // is 0x100, so one LSB keeps the walk finite without standing in
        // for a rate anything asks for.
        uint32_t step = inc[c] != 0 ? inc[c] : 1;
        bool live = capture != nullptr && c == 0 && (ctrl & 1) &&
                    (chan_en & 1);

        for (; frac[c] < AUDIO_INC_UNITY; frac[c] += step)
            if (live) fwrite(b, 1, 4, capture);
        frac[c] -= AUDIO_INC_UNITY;
    }

    // A finite bring-up has ended: what queued meanwhile plays at once.
    void come_up() {
        stage = Stage::ready;
        for (uint32_t c = 0; c < AUDIO_CHANNELS; c++) {
            for (uint32_t w : held[c]) play(c, w);
            held[c].clear();
        }
        update_watermarks();
    }

    // STATUS shares CAP's slot, told apart by address bit 5 alone, so it
    // mirrors into every odd block outside the channels, as the device
    // decodes it.
    static bool is_status(uint32_t addr) {
        return chan_of(addr) == AUDIO_CHANNELS && ((addr >> 2) & 7) == 0 &&
               (addr & 0x20) != 0;
    }

    // Global registers occupy the first 32-byte block, channel n the
    // block at 8 + n — which is what 0x100 + n * 0x20 means.  Returns
    // AUDIO_CHANNELS for an address outside any channel.
    static uint32_t chan_of(uint32_t addr) {
        uint32_t blk = (addr >> 5) & 0x3F;
        return (blk >= 8 && blk < 8 + AUDIO_CHANNELS) ? blk - 8
                                                      : AUDIO_CHANNELS;
    }

    uint32_t read(uint32_t addr) {
        uint32_t r = (addr >> 2) & 7;
        uint32_t c = chan_of(addr);
        if (is_status(addr)) {
            uint32_t st = status();
            if (stage == Stage::busy_for && --busy_reads == 0) come_up();
            return st;
        }
        if (c == AUDIO_CHANNELS) {
            switch (r) {
                case 0: return AUDIO_CAP;
                case 1: return AUDIO_RATE;
                case 2: return ctrl;
                case 3: return chan_en;
                case 4: return irq_status;
                case 5: return irq_enable;
                case 6: return underrun;
                default: return master_vol;
            }
        }
        switch (r) {
            case 1:  return inc[c];
            case 2:  return vol[c];
            case 3:  return AUDIO_DEPTH;
            case 4:  return level(c);    // all free, unless the stage is down
            case 5:  return wm[c];
            default: return 0;
        }
    }

    void write(uint32_t addr, uint32_t data) {
        uint32_t r = (addr >> 2) & 7;
        uint32_t c = chan_of(addr);
        uint32_t mask = (AUDIO_CHANNELS >= 32) ? ~0u
                                               : ((1u << AUDIO_CHANNELS) - 1);
        if (c == AUDIO_CHANNELS) {
            switch (r) {
                case 2: ctrl = data & 1; break;
                case 3: chan_en = data & mask; break;
                case 4: irq_status &= ~data; break;
                case 5: irq_enable = data & mask; break;
                case 6: underrun &= ~data; break;
                case 7: master_vol = data; break;
                default: break;
            }
            return;
        }
        switch (r) {
            case 0:
                // FLUSH discards the queue, and with it where the output
                // sits inside a source frame.
                if (data & (1u << 8)) { frac[c] = 0; held[c].clear(); }
                break;
            case 1: inc[c] = data; break;
            case 2: vol[c] = data; break;
            case 5: wm[c]  = data; break;
            case 6:
                // A frame: consumed at once behind a ready stage, queued
                // behind one that is not, and discarded past a full queue.
                if (stage == Stage::ready) play(c, data);
                else if (held[c].size() < AUDIO_DEPTH) held[c].push_back(data);
                break;
            default: break;
        }
        update_watermarks();
    }

    bool irq() const { return (irq_status & irq_enable) != 0; }
} audio;

// --- Ethernet adapter (CLASS_ETHERNET) ---
// Register contract: doc/system/devices/ethernet.md.  +eth=<backend>
// attaches the device at the end of the autoconfig chain; without it
// the machine has none.  The model manages its own link, which is
// always up, and has no MDIO: it is the class's minimum.
//
// Two backends carry the frames, chosen by
// +eth=backend[,rx=N][,tx=N][,speed=MBIT], with N slots in each ring.
// loop hands every frame sent to the receive side, through the receive
// checks, so a program exercises the whole device with nothing on the
// host.  tap[:name] exchanges frames with a Linux TAP device, where the
// host's own network stack is the peer.
//
// Unlike the audio model this one keeps time, because the contract's
// transmit side is about waiting: a ring that never filled would leave
// TX_WM and the TX interrupt source unreachable.  A frame occupies the
// transmitter for its length on the wire at the link's speed, counted
// in instructions at the timer's rate (TIMER_PRESCALE a microsecond),
// and a looped frame arrives when it has left the wire.  The device is
// brought up to date at each access to it and each time its interrupt
// output is sampled, so what a program observes depends only on how
// many instructions it has run.  The timing orders events; it measures
// nothing.
static constexpr uint32_t ETH_SLOT_SIZE     = 2048;
static constexpr uint32_t ETH_TX_APERTURE   = 0x8000;
static constexpr uint32_t ETH_RX_APERTURE   = 0x10000;
static constexpr uint32_t ETH_RX_SLOTS_MAX  = 1024;
static constexpr uint32_t ETH_MIN_FRAME     = 60;     // without the FCS
static constexpr uint32_t ETH_MAX_FRAME     = 1518;
// What a frame costs on the wire beyond its own bytes: preamble and
// start delimiter, FCS, and the gap before the next frame.
static constexpr uint32_t ETH_WIRE_OVERHEAD = 8 + 4 + 12;
// Instructions between looks at the host's network: a power of two.
static constexpr uint64_t ETH_POLL_INSNS    = 1024;
static constexpr uint32_t ETH_SPEED_MBIT[3] = { 10, 100, 1000 };  // by SPEED code
static constexpr uint32_t ETH_CAP_ADDR      = 1u << 9;
static constexpr uint32_t ETH_CTRL_RX_EN    = 1u << 0, ETH_CTRL_TX_EN    = 1u << 1,
                          ETH_CTRL_PROMISC  = 1u << 2, ETH_CTRL_ALLMULTI = 1u << 3;
static constexpr uint32_t ETH_IRQ_RX        = 1u << 0, ETH_IRQ_TX        = 1u << 1;
static constexpr uint32_t ETH_FACTORY_VALID = 1u << 31;
// Locally administered, so it collides with no real adapter's.
static constexpr uint8_t  ETH_FACTORY_ADDR[6] = { 0x02, 0x50, 0x45, 0x4E, 0x00, 0x01 };
enum : uint32_t {
    ETH_R_CAP = 0x000, ETH_R_IRQ_STATUS = 0x004, ETH_R_IRQ_ENABLE = 0x008,
    ETH_R_CTRL = 0x00C, ETH_R_LINK = 0x010, ETH_R_MDIO = 0x014,
    ETH_R_ADDR_LO = 0x018, ETH_R_ADDR_HI = 0x01C,
    ETH_R_FACTORY_LO = 0x020, ETH_R_FACTORY_HI = 0x024,
    ETH_R_RX_GEOM = 0x040, ETH_R_RX_PROD = 0x044, ETH_R_RX_CONS = 0x048,
    ETH_R_RX_DROP = 0x04C, ETH_R_TX_GEOM = 0x050, ETH_R_TX_PROD = 0x054,
    ETH_R_TX_CONS = 0x058, ETH_R_TX_WM = 0x05C,
};

// A PHY for the adapter that software manages (+eth=...,mdio), as
// clause 22 presents one: the standard registers of a gigabit PHY and
// nothing a vendor adds, so the driver that fits it is the generic
// one.  It answers at management address ETH_PHY_ADDR.  The link
// partner is taken to advertise every mode, so the link settles on the
// best mode the PHY itself advertised when negotiation last started,
// ETH_PHY_ANEG_INSNS after it started.
static constexpr uint32_t ETH_CAP_MDIO   = 1u << 8;
static constexpr uint32_t ETH_MDIO_BUSY  = 1u << 31, ETH_MDIO_WRITE = 1u << 30;
static constexpr uint32_t ETH_MDIO_FIELDS = 0x1F1FFFFFu;   // PHY, REG and DATA
static constexpr int      ETH_PHY_ADDR   = 1;
// A management frame: 64 bits at 2.5 MHz, in instructions.
static constexpr uint64_t ETH_MDIO_INSNS = 64 * TIMER_PRESCALE * 2 / 5;
// Negotiation: 100 ms, long enough that a driver which looks once
// finds no link yet and has to look again.
static constexpr uint64_t ETH_PHY_ANEG_INSNS = 100000 * TIMER_PRESCALE;

struct EthPhy {
    enum : uint16_t {
        BMCR_RESET = 0x8000, BMCR_SPEED100 = 0x2000, BMCR_ANEG = 0x1000,
        BMCR_PDOWN = 0x0800, BMCR_RESTART = 0x0200, BMCR_FDX = 0x0100,
        BMCR_SPEED1000 = 0x0040,
        // 100 and 10 Mbit/s at both duplexes, extended status,
        // negotiation, extended registers.
        BMSR_ABILITY = 0x7909, BMSR_ANEG_DONE = 0x0020, BMSR_LINK = 0x0004,
        ANAR_DEFAULT = 0x01E1, ANAR_100 = 0x0180, ANAR_10 = 0x0060,
        GTCR_1000 = 0x0300,
        // What the partner is taken to advertise, and to have negotiated.
        ANLPAR_ALL = 0x41E1, ANER_PARTNER = 0x0003, GTSR_ALL = 0x3C00,
        EXTSR_1000T = 0x3000,
    };
    static constexpr uint16_t ID1 = 0x5045, ID2 = 0x4E01;   // no vendor's

    uint16_t bmcr, anar, gtcr;
    // What the PHY advertised when negotiation last started: ANAR and
    // GTCR take effect only then.
    uint16_t neg_anar, neg_gtcr;
    uint64_t settle_at;     // negotiation ends at this instruction count
    bool     link_dropped;  // BMSR's link bit reads 0 once after a drop

    void restart(uint64_t now) {
        neg_anar = anar;
        neg_gtcr = gtcr;
        settle_at = now + ETH_PHY_ANEG_INSNS;
        link_dropped = true;
    }
    void power_on(uint64_t now) {
        bmcr = BMCR_ANEG | BMCR_FDX | BMCR_SPEED1000;
        anar = ANAR_DEFAULT;
        gtcr = GTCR_1000;
        restart(now);
    }

    // The speed the link settles on, as a SPEED code, or -1 for none.
    int speed() const {
        if (!(bmcr & BMCR_ANEG))
            return (bmcr & BMCR_SPEED1000) ? 2 : (bmcr & BMCR_SPEED100) ? 1 : 0;
        if (neg_gtcr & GTCR_1000) return 2;
        if (neg_anar & ANAR_100) return 1;
        if (neg_anar & ANAR_10) return 0;
        return -1;
    }
    // Whether the link is established at instruction count `now`.
    bool link_up(uint64_t now) const {
        return !(bmcr & BMCR_PDOWN) && now >= settle_at && speed() >= 0;
    }

    uint16_t read(int reg, uint64_t now) {
        bool up = link_up(now);
        bool negotiated = (bmcr & BMCR_ANEG) && now >= settle_at;
        switch (reg) {
            case 0:  return bmcr;
            case 1: {
                uint16_t v = BMSR_ABILITY | (negotiated ? BMSR_ANEG_DONE : 0) |
                             (up && !link_dropped ? BMSR_LINK : 0);
                link_dropped = !up;
                return v;
            }
            case 2:  return ID1;
            case 3:  return ID2;
            case 4:  return anar;
            case 5:  return negotiated ? ANLPAR_ALL : 0;
            case 6:  return negotiated ? ANER_PARTNER : 0;
            case 9:  return gtcr;
            case 10: return negotiated && (neg_gtcr & GTCR_1000) ? GTSR_ALL : 0;
            case 15: return EXTSR_1000T;
            default: return 0;
        }
    }

    void write(int reg, uint16_t v, uint64_t now) {
        switch (reg) {
            case 0: {
                if (v & BMCR_RESET) { power_on(now); break; }
                uint16_t was = bmcr;
                bmcr = v & ~(BMCR_RESET | BMCR_RESTART);
                if ((v & BMCR_RESTART) || bmcr != was) restart(now);
                break;
            }
            case 4:  anar = v; break;
            case 9:  gtcr = v & GTCR_1000; break;
            default: break;
        }
    }
};

static struct {
    enum class Backend { none, loop, tap };
    Backend  backend  = Backend::none;
    std::string tap_name = "tap0";
    int      tap_fd   = -1;
    uint32_t rx_slots = 32, tx_slots = 8;   // powers of two
    // A SPEED code: the link's on a device that manages its own, and
    // the fastest the device carries on one with a PHY.
    uint32_t speed    = 1;
    // With a PHY, the link is software's to manage: CAP.MDIO.
    bool     has_phy  = false;
    EthPhy   phy;

    uint32_t ctrl, irq_enable, addr_lo, addr_hi, rx_drop;
    // CTRL.SPEED as software wrote it: the speed the device runs at when
    // it has a PHY.  A level.
    uint32_t ctrl_speed;
    // MDIO as last written, DATA replaced by what a read returned.  The
    // transaction it started ends at mdio_done_at.
    uint32_t mdio;
    uint64_t mdio_done_at;
    // Frame counts, each written by one side: the device advances
    // rx_prod and tx_cons, software writes rx_cons and tx_prod.  All
    // four wrap from 65535 to 0, and a ring holds prod - cons frames
    // taken modulo 65536: from 0 up to its slot count.
    uint16_t rx_prod, rx_cons, tx_prod, tx_cons;
    // TX_WM as software wrote it: the TX interrupt source's threshold
    // on the frames queued in the transmit ring.  A level, not an event.
    uint16_t tx_wm;
    std::vector<uint8_t> rx_mem, tx_mem;

    // The transmitter.  While tx_busy, wire[0..wire_len) is on the wire
    // until the instruction count reaches tx_done_at.  tx_flush is set
    // when TX_EN is cleared and stands until the queue is discarded.
    bool     tx_busy, tx_flush;
    uint64_t tx_done_at;
    uint8_t  wire[ETH_SLOT_SIZE];
    uint32_t wire_len;

    // The receive side of the wire, for frames from the host: the
    // instruction count at which the last one to arrive has passed, had
    // it come in at the link's speed.
    uint64_t rx_free_at;

    bool present() const { return backend != Backend::none; }

    void reset() {
        ctrl = irq_enable = addr_lo = addr_hi = rx_drop = 0;
        ctrl_speed = mdio = 0;
        mdio_done_at = 0;
        rx_prod = rx_cons = tx_prod = tx_cons = tx_wm = 0;
        tx_busy = tx_flush = false;
        tx_done_at = 0;
        wire_len = 0;
        rx_free_at = 0;
    }

    // "+eth=loop|tap[:name][,mdio][,rx=N][,tx=N][,speed=10|100|1000]",
    // N a power of two.  False on anything else, which the caller
    // refuses.
    bool configure(const char* spec) {
        const char* p = spec;
        Backend chosen;
        if (strncmp(p, "loop", 4) == 0) {
            chosen = Backend::loop;
            p += 4;
        } else if (strncmp(p, "tap", 3) == 0) {
            chosen = Backend::tap;
            p += 3;
            if (*p == ':') {
                size_t n = strcspn(++p, ",");
                if (n == 0 || n >= IFNAMSIZ) return false;
                tap_name.assign(p, n);
                p += n;
            }
        } else {
            return false;
        }
        uint32_t rx = rx_slots, tx = tx_slots, sp = speed;
        bool with_phy = false;
        while (*p == ',') {
            if (strncmp(p, ",mdio", 5) == 0 && (p[5] == ',' || p[5] == '\0')) {
                with_phy = true;
                p += 5;
                continue;
            }
            const char* value = strchr(p, '=');
            if (!value) return false;
            char* end;
            unsigned long n = strtoul(++value, &end, 10);
            if (end == value) return false;
            bool pow2 = n != 0 && (n & (n - 1)) == 0;
            if (strncmp(p, ",rx=", 4) == 0) {
                if (!pow2 || n > ETH_RX_SLOTS_MAX) return false;
                rx = uint32_t(n);
            } else if (strncmp(p, ",tx=", 4) == 0) {
                if (!pow2 || n * ETH_SLOT_SIZE > ETH_RX_APERTURE - ETH_TX_APERTURE)
                    return false;
                tx = uint32_t(n);
            } else if (strncmp(p, ",speed=", 7) == 0) {
                sp = 0;
                while (sp < 3 && ETH_SPEED_MBIT[sp] != n) sp++;
                if (sp == 3) return false;
            } else {
                return false;
            }
            p = end;
        }
        if (*p != '\0') return false;

        backend = chosen;
        rx_slots = rx; tx_slots = tx; speed = sp;
        // The PHY powers up with the machine, and no reset of the device
        // or the bus touches it afterwards.
        has_phy = with_phy;
        if (has_phy) phy.power_on(0);
        rx_mem.assign(rx * ETH_SLOT_SIZE, 0);
        tx_mem.assign(tx * ETH_SLOT_SIZE, 0);
        // The window reaches the end of the receive ring.
        AcfgSlot& slot = acfg.slot[ACFG_SLOT_ETH];
        slot.absent = false;
        slot.size = ETH_RX_APERTURE;
        while (slot.size < ETH_RX_APERTURE + rx * ETH_SLOT_SIZE) slot.size <<= 1;
        return true;
    }

    // Opens the TAP device the tap backend names.  The device may exist
    // already, set up by whoever owns the network it is on, or be
    // created here; either takes the right to administer that network.
    bool attach() {
        if (backend != Backend::tap) return true;
#ifdef __linux__
        int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", tap_name.c_str());
        if (fd < 0 || ioctl(fd, TUNSETIFF, &ifr) < 0) {
            fprintf(stderr, "penumbra-iss: +eth=tap: cannot open TAP device "
                    "'%s': %s\n  (sw/tools/iss-net.sh runs a command where "
                    "one can be made without root)\n",
                    tap_name.c_str(), strerror(errno));
            if (fd >= 0) close(fd);
            return false;
        }
        tap_fd = fd;
        return true;
#else
        fprintf(stderr, "penumbra-iss: +eth=tap needs a Linux host\n");
        return false;
#endif
    }

    // A frame that has left the wire reaches whatever is on it.  A TAP
    // device that cannot take the frame loses it, as a wire would.
    void deliver() {
        if (backend == Backend::loop) {
            receive(wire, wire_len);
        } else if (backend == Backend::tap) {
            if (::write(tap_fd, wire, wire_len) < 0) { /* lost */ }
        }
    }

    // Whether the next frame the host has sent may arrive now.  The
    // host hands frames over far faster than the link the model
    // presents could carry them.  Arriving at the link's pace, they
    // overrun the receive ring only when the guest is slower than its
    // link, which is when a device on a wire would be overrun too.
    bool tap_may_deliver(uint64_t now) const {
        return now >= rx_free_at;
    }

    // Takes the frames the host has sent, as far as they may arrive.
    // The host does not pad a short frame as a sender on a wire has
    // to, so that is done here.
    void poll_host() {
        if (backend != Backend::tap) return;
        service();
        uint8_t frame[ETH_SLOT_SIZE];
        while (tap_may_deliver(cpu.insn_count)) {
            ssize_t n = ::read(tap_fd, frame, sizeof frame);
            if (n <= 0) break;
            uint32_t len = uint32_t(n);
            if (len < ETH_MIN_FRAME) {
                memset(frame + len, 0, ETH_MIN_FRAME - len);
                len = ETH_MIN_FRAME;
            }
            receive(frame, len);
            // A frame found waiting may have reached the host's side at
            // any point since the last look, so it is taken to follow
            // the one before it from no later than that: the pace is
            // the link's, not the polling's.
            uint64_t now = cpu.insn_count;
            uint64_t last_look = now > ETH_POLL_INSNS ? now - ETH_POLL_INSNS : 0;
            rx_free_at = std::max(rx_free_at, last_look) + wire_insns(len);
        }
    }

    // IRQ_STATUS: ETH_IRQ_RX and ETH_IRQ_TX, each a condition on the
    // counts above ("IRQ_STATUS" in the contract).  Read-only, and
    // independent of IRQ_ENABLE.
    uint32_t irq_status() const {
        // The narrowing takes the difference modulo 65536.
        uint16_t tx_pending = tx_prod - tx_cons;

        uint32_t status = 0;
        if (tx_pending < tx_wm)
            status |= ETH_IRQ_TX;
        if (rx_cons != rx_prod)
            status |= ETH_IRQ_RX;
        return status;
    }

    // The device's interrupt output, sampled before each instruction.
    bool irq() {
        if (!present()) return false;
        if (tx_busy && cpu.insn_count >= tx_done_at) service();
        return (irq_status() & irq_enable) != 0;
    }

    // Whether the link is ready to pass the device's frames at
    // instruction count `now`.  A device that manages its own link
    // always has one that is.  With a PHY the link has to be up and
    // the device running at its speed: until software has written
    // that speed to CTRL.SPEED, in the contract's words, "frames do
    // not get through".
    bool link_ready(uint64_t now) const {
        if (!has_phy)
            return true;
        if (!phy.link_up(now))
            return false;
        if ((uint32_t)phy.speed() != ctrl_speed)
            return false;
        return true;
    }

    // Instructions a frame of len bytes occupies the wire for, at the
    // speed the device runs at.
    uint64_t wire_insns(uint32_t len) const {
        uint64_t bits = uint64_t(len + ETH_WIRE_OVERHEAD) * 8;
        uint32_t mbit = ETH_SPEED_MBIT[has_phy ? ctrl_speed : speed];
        return (bits * TIMER_PRESCALE + mbit - 1) / mbit;
    }

    // Whether a frame to this destination is addressed to the station.
    bool accepts(const uint8_t* dst) const {
        static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (ctrl & ETH_CTRL_PROMISC) return true;
        if (dst[0] & 1)     // a group address
            return (ctrl & ETH_CTRL_ALLMULTI) || memcmp(dst, broadcast, 6) == 0;
        const uint8_t station[6] = {
            uint8_t(addr_lo), uint8_t(addr_lo >> 8), uint8_t(addr_lo >> 16),
            uint8_t(addr_lo >> 24), uint8_t(addr_hi), uint8_t(addr_hi >> 8) };
        return memcmp(dst, station, 6) == 0;
    }

    // A frame arriving from the wire, without preamble or FCS.
    void receive(const uint8_t* frame, uint32_t len) {
        if (!link_ready(cpu.insn_count)) return;
        if (!(ctrl & ETH_CTRL_RX_EN)) return;
        if (len < ETH_MIN_FRAME || len > ETH_MAX_FRAME) return;
        if (!accepts(frame)) return;
        if (uint16_t(rx_prod - rx_cons) >= rx_slots) { rx_drop++; return; }
        uint8_t* slot = &rx_mem[(rx_prod % rx_slots) * ETH_SLOT_SIZE];
        slot[0] = uint8_t(len);
        slot[1] = uint8_t(len >> 8);
        memcpy(slot + 2, frame, len);
        rx_prod++;
    }

    // Brings the transmitter up to date: ends each frame whose time on
    // the wire is over, discards the queue once TX_EN has been cleared,
    // and starts the next queued frame.  Frames queued back to back go
    // out back to back, however late the device is brought up to date.
    void service() {
        uint64_t now = cpu.insn_count;
        uint64_t free_at = now;     // when the transmitter last became free
        for (;;) {
            if (tx_busy) {
                if (now < tx_done_at) return;
                tx_busy = false;
                free_at = tx_done_at;
                tx_cons++;
                if (link_ready(now)) deliver();
            }
            if (tx_flush) { tx_cons = tx_prod; tx_flush = false; }
            if (!(ctrl & ETH_CTRL_TX_EN) || tx_cons == tx_prod) return;

            const uint8_t* slot = &tx_mem[(tx_cons % tx_slots) * ETH_SLOT_SIZE];
            uint32_t len = slot[0] | (uint32_t(slot[1]) << 8);
            if (len == 0 || len > ETH_SLOT_SIZE - 2) {  // sends nothing
                tx_cons++;
                continue;
            }
            memcpy(wire, slot + 2, len);
            if (len < ETH_MIN_FRAME) {
                memset(wire + len, 0, ETH_MIN_FRAME - len);
                len = ETH_MIN_FRAME;
            }
            wire_len = len;
            tx_busy = true;
            tx_done_at = free_at + wire_insns(len);
        }
    }

    // The slot apertures behave as plain memory: an access of 1 << size
    // bytes at its natural alignment, little-endian.
    uint32_t aperture_read(const std::vector<uint8_t>& mem, uint32_t p, int size) const {
        uint32_t n = 1u << size, v = 0;
        p &= ~(n - 1);
        for (uint32_t i = 0; i < n; i++)
            if (p + i < mem.size()) v |= uint32_t(mem[p + i]) << (8 * i);
        return v;
    }
    void aperture_write(std::vector<uint8_t>& mem, uint32_t p, uint32_t data, int size) {
        uint32_t n = 1u << size;
        p &= ~(n - 1);
        for (uint32_t i = 0; i < n; i++)
            if (p + i < mem.size()) mem[p + i] = uint8_t(data >> (8 * i));
    }

    uint32_t read(uint32_t addr, int size) {
        service();
        uint32_t off = addr & (acfg.slot[ACFG_SLOT_ETH].size - 1);
        if (off >= ETH_RX_APERTURE)
            return aperture_read(rx_mem, off - ETH_RX_APERTURE, size);
        if (off >= ETH_TX_APERTURE) return 0;   // write-only
        switch (off & ~3u) {
            case ETH_R_CAP:
                // A device with a PHY carries every speed up to its own.
                return 1u | ETH_CAP_ADDR |
                       (has_phy ? ETH_CAP_MDIO | (((2u << speed) - 1) << 16)
                                : 1u << (16 + speed));
            case ETH_R_IRQ_STATUS: return irq_status();
            case ETH_R_IRQ_ENABLE: return irq_enable;
            case ETH_R_CTRL:       return ctrl | (ctrl_speed << 8);
            case ETH_R_LINK:       return has_phy ? 0 : 1u | (speed << 8);
            case ETH_R_MDIO:
                if (!has_phy) return 0;
                return mdio | (cpu.insn_count < mdio_done_at ? ETH_MDIO_BUSY : 0);
            case ETH_R_ADDR_LO:    return addr_lo;
            case ETH_R_ADDR_HI:    return addr_hi;
            case ETH_R_FACTORY_LO:
                return ETH_FACTORY_ADDR[0] | (ETH_FACTORY_ADDR[1] << 8) |
                       (ETH_FACTORY_ADDR[2] << 16) | (uint32_t(ETH_FACTORY_ADDR[3]) << 24);
            case ETH_R_FACTORY_HI:
                return ETH_FACTORY_ADDR[4] | (ETH_FACTORY_ADDR[5] << 8) | ETH_FACTORY_VALID;
            case ETH_R_RX_GEOM:    return rx_slots | (ETH_SLOT_SIZE << 16);
            case ETH_R_RX_PROD:    return rx_prod;
            case ETH_R_RX_CONS:    return rx_cons;
            case ETH_R_RX_DROP:    return rx_drop;
            case ETH_R_TX_GEOM:    return tx_slots | (ETH_SLOT_SIZE << 16);
            case ETH_R_TX_PROD:    return tx_prod;
            case ETH_R_TX_CONS:    return tx_cons;
            case ETH_R_TX_WM:      return tx_wm;
            default:               return 0;    // reserved
        }
    }

    void write(uint32_t addr, uint32_t data, int size) {
        service();
        uint32_t off = addr & (acfg.slot[ACFG_SLOT_ETH].size - 1);
        if (off >= ETH_RX_APERTURE) return;     // read-only
        if (off >= ETH_TX_APERTURE) {
            aperture_write(tx_mem, off - ETH_TX_APERTURE, data, size);
            return;
        }
        switch (off & ~3u) {
            case ETH_R_IRQ_ENABLE: irq_enable = data & (ETH_IRQ_RX | ETH_IRQ_TX); break;
            case ETH_R_CTRL: {
                bool sending = ctrl & ETH_CTRL_TX_EN;
                ctrl = data & (ETH_CTRL_RX_EN | ETH_CTRL_TX_EN |
                               ETH_CTRL_PROMISC | ETH_CTRL_ALLMULTI);
                if (sending && !(ctrl & ETH_CTRL_TX_EN)) tx_flush = true;
                // SPEED takes only a speed the device carries.
                uint32_t sp = (data >> 8) & 3;
                if (has_phy && sp <= speed) ctrl_speed = sp;
                break;
            }
            case ETH_R_MDIO: {
                uint64_t now = cpu.insn_count;
                if (!has_phy || now < mdio_done_at) break;  // absent, or busy
                int phy_addr = (data >> 24) & 0x1F, reg = (data >> 16) & 0x1F;
                mdio = data & (ETH_MDIO_WRITE | ETH_MDIO_FIELDS);
                if (data & ETH_MDIO_WRITE) {
                    if (phy_addr == ETH_PHY_ADDR) phy.write(reg, uint16_t(data), now);
                } else {
                    // Nothing answers at any other address, and the
                    // line's pull-up reads as ones.
                    uint16_t v = phy_addr == ETH_PHY_ADDR ? phy.read(reg, now) : 0xFFFF;
                    mdio = (mdio & ~0xFFFFu) | v;
                }
                mdio_done_at = now + ETH_MDIO_INSNS;
                break;
            }
            case ETH_R_ADDR_LO:    addr_lo = data; break;
            case ETH_R_ADDR_HI:    addr_hi = data & 0xFFFF; break;
            case ETH_R_RX_CONS:    rx_cons = uint16_t(data); break;
            case ETH_R_TX_PROD:    tx_prod = uint16_t(data); break;
            case ETH_R_TX_WM:      tx_wm = uint16_t(data); break;
            default: break;
        }
        service();
    }
} eth;

// --- Display adapter (CLASS_DISPLAY, character console) ---
// Register contract: doc/system/devices/display.md; the executable
// spec is hw/sim/tb_video_display.cpp — behaviors pinned there hold
// here. Storage-level model: registers and the cell array, no
// rendering; Ctrl-A D dumps the screen as text.
static constexpr uint32_t DISPLAY_FB_W = 320, DISPLAY_FB_H = 240;
static constexpr uint32_t DISPLAY_CAP  =
    (1u << 16) | (1u << 13) | (1u << 10) | (1u << 8) | 1u;
static constexpr uint32_t DISPLAY_INFO = (30u << 16) | 80u;
static constexpr uint32_t DISPLAY_FB_GEOM =
    (DISPLAY_FB_H << 16) | DISPLAY_FB_W;
static constexpr uint32_t DISPLAY_FB_FORMAT = 8;   // 8bpp, palette-indexed

static struct {
    uint32_t ctrl;          // [0] ENABLE (resets 0), [1] CURSOR_EN, [4] FB_SEL
    uint32_t cursor;        // full written word, faithful readback
    uint16_t cells[4096];
    uint8_t  fb[DISPLAY_FB_W * DISPLAY_FB_H];
    uint32_t fbpal[256];

    // Cells and pixels are not cleared: the contract leaves their
    // power-up contents undefined, and software owns screen content.
    void reset() { ctrl = 0; cursor = 0; }

    // Region split, matching the device: registers and the cell
    // aperture below 0x10000, the framebuffer palette at 0x10000, the
    // pixel aperture from 0x20000. The pixel aperture is the one that
    // behaves as plain memory, so it is also the only one that takes
    // sub-word access; everything else is word-strided.
    bool fb_region(uint32_t off)  const { return (off & 0x20000) != 0; }
    bool pal_region(uint32_t off) const { return (off & 0x30000) == 0x10000; }

    uint32_t read(uint32_t off, int size) const {
        off &= 0x3FFFF;
        if (fb_region(off)) {
            uint32_t p = off & 0x1FFFF;
            if (size == 0) return p < sizeof fb ? fb[p] : 0;
            p &= (size == 1) ? ~1u : ~3u;
            uint32_t n = 1u << size, v = 0;
            for (uint32_t i = 0; i < n; i++)
                if (p + i < sizeof fb) v |= (uint32_t)fb[p + i] << (8 * i);
            return v;
        }
        if (pal_region(off))
            return fbpal[(off >> 2) & 0xFF];
        if (off & 0xF000)
            return cells[((off >> 2) - 1024) & 0xFFF];
        switch ((off >> 2) & 0xF) {
            case 0: return DISPLAY_CAP;
            case 1: return DISPLAY_INFO;
            case 2: return ctrl;
            case 3: return cursor;
            case 7: return DISPLAY_FB_GEOM;
            case 8: return DISPLAY_FB_FORMAT;
            default: return 0;   // modes absent
        }
    }

    void write(uint32_t off, uint32_t data, int size) {
        off &= 0x3FFFF;
        if (fb_region(off)) {
            uint32_t p = off & 0x1FFFF;
            if (size == 0) { if (p < sizeof fb) fb[p] = (uint8_t)data; return; }
            p &= (size == 1) ? ~1u : ~3u;
            uint32_t n = 1u << size;
            for (uint32_t i = 0; i < n; i++)
                if (p + i < sizeof fb) fb[p + i] = (uint8_t)(data >> (8 * i));
            return;
        }
        if (pal_region(off)) {
            fbpal[(off >> 2) & 0xFF] = data & 0x00FFFFFF;
            return;
        }
        if (off & 0xF000) {
            cells[((off >> 2) - 1024) & 0xFFF] = (uint16_t)data;
            return;
        }
        switch ((off >> 2) & 0xF) {
            case 2: ctrl = data & 0x13; break;
            case 3: cursor = data; break;
            default: break;      // read-only and absent slots ignore writes
        }
    }
} display;

// --- Main TLB: slot way * sets + set, for the presented geometry ---
static struct {
    std::vector<uint32_t> vpn;   // {4'b0, VPN[19:0], ASID[7:0]}
    std::vector<uint32_t> pte;   // {PPN[19:0], SW[3:0], flags[7:0]}
} tlb;

// --- Pinned TLB (8 entries, fully associative) ---
static struct {
    uint32_t vpn[PTLB_ENTRIES];
    uint32_t pte[PTLB_ENTRIES];
} ptlb;

// --- MMU state ---
static struct {
    uint32_t cr;          // [0]=M enable, [15:8]=ASID
    uint32_t fault_addr;
    uint32_t fault_status;
    uint32_t tlb_idx;     // pinned select [6] (v1) or [31] (v2), slot below
    uint32_t tlb_vpn_reg; // Shared staging register
    bool     enabled() const { return cr & 1; }
    uint8_t  asid()    const { return (cr >> 8) & 0xFF; }
} mmu;

// The hardware generation the ISS presents (+core=), set before reset.
static const CoreGeneration* core_gen;

// --- Sysreg: System ID (device 1, read-only) ---
static uint32_t cpuid_read(int reg) {
    switch (reg) {
        case 0: return 1;            // CPU_ISA: version 1
        // "Penumbra" + the generation tag, 16 bytes packed LE, null-padded
        case 1: return 0x756E6550;   // "Penu"
        case 2: return 0x6172626D;   // "mbra"
        case 3: return core_gen->cpu_name2;
        case 4: return 0;
        // The ISS is instruction-accurate, not cycle-accurate.  We expose
        // insn_count for both registers so the bench harness can read
        // them without special-casing — IPC will always be 1 on the ISS,
        // which is clearly synthetic.  Real cycle numbers come from RTL
        // simulation or FPGA execution.
        case 5: return (uint32_t)cpu.insn_count;  // CPU_CYCLES (synthetic on ISS)
        case 6: return (uint32_t)cpu.insn_count;  // CPU_INSNS_RETIRED
        // Stall-attribution counters (regs 7-10) are cycle-accurate by
        // nature; the ISS does not model stalls, so they read 0.
        case 7: case 8: case 9: case 10: return 0;
        default: return 0;
    }
}

// --- Sysreg: the caches (devices 2, 3 and 9) ---
// The ISS holds no cache contents; caches are performance-transparent.
// What it presents is what software reads: the generation's geometry
// (INFO) and the enable software wrote (CTRL), so the kernel's cache
// bring-up sees the machine it would run on.  STATUS never reads busy
// and the event counters read 0.
struct CacheDevice {
    const CacheGeometry* geometry;
    bool                 enabled;
};
static CacheDevice cache_l1d, cache_l1i, cache_l2;

static CacheDevice& cache_device(int dev) {
    switch (dev) {
        case SYSDEV_L1_DCACHE: return cache_l1d;
        case SYSDEV_L1_ICACHE: return cache_l1i;
        default:               return cache_l2;
    }
}

static void cache_devices_reset() {
    cache_l1d = { &core_gen->l1d, false };
    cache_l1i = { &core_gen->l1i, false };
    cache_l2  = { &core_gen->l2,  false };
}

static uint32_t cache_read(int dev, int reg) {
    const CacheDevice& c = cache_device(dev);
    switch (reg) {
        case CACHE_REG_INFO: return c.geometry->info();
        case CACHE_REG_CTRL: return c.enabled ? 1 : 0;
        default:             return 0;
    }
}

// +cachemodel: shadow hierarchies fed by every access (cache_model.h).
static CacheModel* cache_model;

// +tlbmodel: shadow TLBs fed by every main-TLB translation (tlb_model.h).
static TlbModel* tlb_model;

// +sbmodel: shadow store buffers fed by every store and every fill
// (sb_model.h).
static SbModel* sb_model;

// +bpmodel: shadow branch predictors fed by every control transfer
// (bp_model.h).
static BpModel* bp_model;

// +pairmodel: shadow issue stages fed by every instruction that reaches
// the end of its execution (pair_model.h).
static PairModel* pair_model;

// +loadusemodel: shadow single-issue pipelines fed by every instruction
// that reaches the end of its execution (loaduse_model.h).
static LoadUseModel* loaduse_model;

// +opstats: an instruction-class histogram and a MUL/DIV operand census,
// counted over each window as the models' counts are.
static bool opstats_enabled = false;
static void opstats_clear();
static void opstats_report(FILE* out);
static void opstats_exception(int vector);

static CacheEnables cache_enables() {
    return { cache_l1i.enabled, cache_l1d.enabled, cache_l2.enabled };
}

static void cache_write(int dev, int reg, uint32_t val) {
    if (reg == CACHE_REG_CTRL) cache_device(dev).enabled = val & 1;
    if (reg == CACHE_REG_INVAL_ALL && cache_model)
        cache_model->invalidate(dev == SYSDEV_L1_ICACHE ? CacheLevel::L1I
                              : dev == SYSDEV_L1_DCACHE ? CacheLevel::L1D : CacheLevel::L2);
}

static bool supervisor() { return cpu.sr & SR_S; }

// --- Measurement window (+model_start, +model_stop, SIGUSR1) ---
// The models watch the whole run, so their state warms from reset as the
// hardware's would, but they count only inside a window and report each
// window as it closes; +opstats counts and reports by the same windows.
// A window opens at reset, or at the first user-mode execution of
// +model_start; it closes at the first user-mode execution of +model_stop,
// on SIGUSR1 (which opens the next window at once), or when the run ends.
// User mode, as for +pcprof_start: the workload's own code, not the kernel
// passing the same address on its behalf.
static struct {
    uint32_t    start_pc, stop_pc;
    bool        open, finished;
    const char* opened_by;
    uint64_t    insn0;
    int         number;
    FILE*       out;
} model_window;
static volatile sig_atomic_t model_window_signal = 0;

// Whether anything counts by window: a model, or +opstats.
static bool windows_active() {
    return opstats_enabled || cache_model || tlb_model || sb_model || bp_model || pair_model
        || loaduse_model;
}

static void model_window_open(const char* why) {
    if (opstats_enabled) opstats_clear();
    if (cache_model) cache_model->clear_counts();
    if (tlb_model) tlb_model->clear_counts();
    if (sb_model) sb_model->clear_counts();
    if (bp_model) bp_model->clear_counts();
    if (pair_model) pair_model->clear_counts();
    if (loaduse_model) loaduse_model->clear_counts();
    model_window.open      = true;
    model_window.opened_by = why;
    model_window.insn0     = cpu.insn_count;
    model_window.number++;
}

static void model_window_close(const char* why) {
    FILE* out = model_window.out;
    uint64_t insns = cpu.insn_count - model_window.insn0;
    fprintf(out, "== model window %d: %llu instructions, %s to %s ==\n",
            model_window.number, (unsigned long long)insns, model_window.opened_by, why);
    if (opstats_enabled) opstats_report(out);
    if (cache_model) cache_model->report(out, insns);
    if (tlb_model) tlb_model->report(out, insns, mmu_geometry);
    if (sb_model) sb_model->report(out, insns);
    if (bp_model) bp_model->report(out, insns);
    if (pair_model) {
        pair_model->end_window();
        pair_model->report(out);
    }
    if (loaduse_model) loaduse_model->report(out);
    fflush(out);
    model_window.open = false;
}

static void model_window_step() {
    if (model_window_signal) {
        model_window_signal = 0;
        if (model_window.open) {
            model_window_close("signal");
            model_window_open("signal");
        }
    }
    if (supervisor() || model_window.finished) return;
    if (!model_window.open && cpu.pc == model_window.start_pc)
        model_window_open("start PC");
    else if (model_window.open && model_window.stop_pc && cpu.pc == model_window.stop_pc) {
        model_window_close("stop PC");
        model_window.finished = true;
    }
}

static void model_window_on_signal(int) { model_window_signal = 1; }

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

// +irq_at_pc=ADDR[,PERIOD] — raise the external IRQ line when the CPU is
// about to execute ADDR.  Real interrupts arrive only from modelled
// devices, which cannot be aimed at an instruction, so this is how a
// short instruction window gets interrupted deterministically rather
// than once in millions of ticks.
//
// PERIOD fires on every Nth deliverable arrival.  A restartable sequence
// needs at least 2: rewinding to its start brings the CPU back to ADDR,
// so firing on every arrival livelocks it.
static uint32_t irq_at_pc_addr = 0;
static uint64_t irq_at_pc_period = 1;
static uint64_t irq_at_pc_arrivals = 0;
static uint64_t irq_at_pc_injected = 0;

// +halt_on=STR — stop the sim the moment STR appears in the UART output
// stream.  Mirrors the RTL console's +halt_on so the same marker (e.g. a
// panic banner) halts both simulators identically.
static const char* halt_on_str = nullptr;
static size_t      halt_on_len = 0;
static char        halt_on_buf[256];   // rolling tail of emitted UART bytes
static size_t      halt_on_pos = 0;
static bool        halt_on_hit = false;

static void halt_on_feed(char c) {
    if (!halt_on_str || halt_on_hit) return;
    if (halt_on_pos < halt_on_len) {
        halt_on_buf[halt_on_pos++] = c;
    } else {
        memmove(halt_on_buf, halt_on_buf + 1, halt_on_len - 1);
        halt_on_buf[halt_on_len - 1] = c;
    }
    if (halt_on_pos == halt_on_len &&
        memcmp(halt_on_buf, halt_on_str, halt_on_len) == 0)
        halt_on_hit = true;
}

// +trace_window=N — keep only the last N traced instructions in a ring buffer
// and flush them on exit, instead of streaming every line to disk.  Makes it
// feasible to trace a multi-billion-instruction run down to just the window
// before a halt (e.g. +halt_on=DBLFLT), without filling the disk.
struct TraceRec { uint32_t pc, sr, r[15]; };  // r[1..14] used
static size_t trace_window = 0;
static std::vector<TraceRec> trace_ring;
static size_t trace_ring_head = 0;
static size_t trace_ring_fill = 0;

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
// Returns true on hit (paddr/cacheable/pinned/global valid), false on miss.
// Sets fault=true if hit but permissions deny access.
static bool tlb_lookup(uint32_t vaddr, uint8_t asid, uint8_t access,
                       bool user_mode, uint32_t& paddr, bool& cacheable,
                       bool& pinned, bool& global, bool& fault) {
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
        global              = pte_word & TLB_G;

        if (entry_vpn != va_vpn) continue;
        if (!global && entry_asid != asid) continue;

        fault = (pte_word & need_mask) != need_mask;
        paddr = (TLB_PTE_GET_PPN(pte_word) << 12) | (vaddr & 0xFFF);
        cacheable = pte_word & TLB_C;
        pinned = true;
        return true;
    }

    // Fall through to main TLB (set-associative)
    uint32_t set = va_vpn & (mmu_geometry.sets - 1);
    for (uint32_t way = 0; way < mmu_geometry.ways; way++) {
        uint32_t idx = way * mmu_geometry.sets + set;
        uint32_t vpn_word = tlb.vpn[idx];
        uint32_t pte_word = tlb.pte[idx];

        if (!(pte_word & TLB_V)) continue;

        uint32_t entry_vpn  = TLB_VPN_GET_VPN(vpn_word);
        uint8_t  entry_asid = TLB_VPN_GET_ASID(vpn_word);
        global              = pte_word & TLB_G;

        if (entry_vpn != va_vpn) continue;
        if (!global && entry_asid != asid) continue;

        fault = (pte_word & need_mask) != need_mask;
        paddr = (TLB_PTE_GET_PPN(pte_word) << 12) | (vaddr & 0xFFF);
        cacheable = pte_word & TLB_C;
        pinned = false;
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
// MMU Translation
// ═══════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════
// Execution profile (+pcprof)
// ═══════════════════════════════════════════════════════════════
//
// Buckets executed instructions by PC, exceptions by vector and TLB
// misses by faulting page, and writes all three to a file at exit.
// Where +opstats says which *kinds* of instructions a run executes,
// this says *where* they are, which is what turns a disassembly into a
// weighted profile: `sw/tools/pcprof-report.py' symbolizes the result.
//
// The counts are architectural. The ISS models no cache or memory
// latency, so a profile weights work by instructions executed and never
// by cycles -- two rows of equal weight can cost very different time on
// hardware.
//
// Buckets are keyed by virtual address, so kernel and user PCs separate
// on their own but two processes sharing a virtual address do not:
// profile one workload at a time.
static const char* pcprof_path = nullptr;
static uint32_t pcprof_start_pc = 0;   // +pcprof_start: defer until this PC
static bool pcprof_on = false;

// One block of counters per 4 KB page of address space, allocated when
// the run first executes there, so an address space nothing runs in
// costs a null pointer rather than a counter.
static std::vector<uint64_t*> pcprof_pages;
static std::map<uint32_t, uint64_t> pcprof_miss;   // TLB misses by faulting VPN
static uint64_t pcprof_vector[16];                 // exceptions by vector
static uint64_t pcprof_insns, pcprof_super_insns;

static inline void pcprof_count_insn(uint32_t pc, bool supervisor) {
    uint64_t*& page = pcprof_pages[pc >> 12];
    if (!page) page = (uint64_t*)calloc(1024, sizeof(uint64_t));
    page[(pc >> 2) & 1023]++;
    pcprof_insns++;
    if (supervisor) pcprof_super_insns++;
}

// Written at exit; one record per line, prefixed by its kind:
//   V <vector> <count>   exceptions taken, by vector number
//   M <vpn> <count>      TLB misses, by faulting page (hex VPN)
//   P <pc> <count>       instructions executed, by PC (hex)
static void pcprof_write() {
    if (!pcprof_path || !pcprof_insns) return;

    FILE* f = fopen(pcprof_path, "w");
    if (!f) {
        fprintf(stderr, "[PCPROF] cannot write '%s'\n", pcprof_path);
        return;
    }
    fprintf(f, "# penumbra-iss execution profile\n");
    fprintf(f, "# insns %llu super %llu\n",
            (unsigned long long)pcprof_insns,
            (unsigned long long)pcprof_super_insns);
    for (int v = 0; v < 16; v++)
        if (pcprof_vector[v])
            fprintf(f, "V %d %llu\n", v, (unsigned long long)pcprof_vector[v]);
    for (const auto& m : pcprof_miss)
        fprintf(f, "M %08x %llu\n", m.first << 12,
                (unsigned long long)m.second);
    for (size_t p = 0; p < pcprof_pages.size(); p++) {
        if (!pcprof_pages[p]) continue;
        for (int i = 0; i < 1024; i++)
            if (pcprof_pages[p][i])
                fprintf(f, "P %08x %llu\n",
                        (uint32_t)(p << 12) | (uint32_t)(i << 2),
                        (unsigned long long)pcprof_pages[p][i]);
    }
    fclose(f);

    uint64_t misses = 0, exceptions = 0;
    for (const auto& m : pcprof_miss) misses += m.second;
    for (int v = 0; v < 16; v++) exceptions += pcprof_vector[v];

    fprintf(stderr, "\n── execution profile (+pcprof) ──\n");
    fprintf(stderr, "  counted %llu instructions, %.1f%% in supervisor mode\n",
            (unsigned long long)pcprof_insns,
            100.0 * (double)pcprof_super_insns / (double)pcprof_insns);
    fprintf(stderr, "  %llu exceptions, of them %llu TLB misses over %zu pages\n",
            (unsigned long long)exceptions, (unsigned long long)misses,
            pcprof_miss.size());
    fprintf(stderr, "  written to '%s'\n", pcprof_path);
}

// Returns: 0=ok, VEC_TLB_MISS, VEC_TLB_PROT, VEC_ALIGN, VEC_BUS_FAULT
static int mmu_translate(uint32_t vaddr, uint8_t access, int size,
                         bool force_bypass, uint32_t& paddr, bool& cacheable) {
    // Alignment check (always, even in bypass)
    if ((size == 2 && (vaddr & 3)) || (size == 1 && (vaddr & 1))) {
        mmu.fault_addr = vaddr;
        bool user = !(cpu.sr & SR_S);
        mmu.fault_status = ((uint32_t)user << 11) | ((uint32_t)access << 8) | 0x03;
        return VEC_ALIGN;
    }

    if (!mmu.enabled() || force_bypass) {
        paddr = vaddr;
        cacheable = false;      // bypass is uncached
        return 0;
    }

    bool pinned, global, fault;
    bool hit = tlb_lookup(vaddr, mmu.asid(), access, !(cpu.sr & SR_S),
                          paddr, cacheable, pinned, global, fault);
    if (!hit) {
        if (tlb_model) tlb_model->real_miss(supervisor());
        mmu.fault_addr = vaddr;
        mmu.fault_status = (uint32_t)((!(cpu.sr&SR_S))<<11) | ((uint32_t)access<<8) | 0x01;
        return VEC_TLB_MISS;
    }
    if (fault) {
        mmu.fault_addr = vaddr;
        mmu.fault_status = (uint32_t)((!(cpu.sr&SR_S))<<11) | ((uint32_t)access<<8) | 0x02;
        return VEC_TLB_PROT;
    }
    if (tlb_model && !pinned) tlb_model->translate(vaddr >> 12, mmu.asid(), global, supervisor());
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
            else { putchar(val); fflush(stdout); uart.thre_int = false; uart.tx_busy_count = 2; halt_on_feed((char)val); }
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

// Autoconfig config space read — the presented slot's identity
static uint32_t acfg_config_read(const AcfgSlot& s, uint32_t addr) {
    int reg = (addr >> 2) & 7;
    switch (reg) {
        case 0: return s.dev_class;
        case 1: return s.size;
        case 2: return 0;        // ID
        case 3: return s.name0;
        case 4: return s.name1;
        case 5: case 6: return 0;  // NAME2-3
        case 7: return 0;        // BASE (write-only)
    }
    return 0;
}

// ── USB host controller model ──────────────────────────────────

// Complete wire image of a token packet: PID byte, then the 11-bit
// field split low-byte-first with CRC5 in the top bits.
static std::vector<uint8_t> usb_token_bytes(uint8_t pid, uint16_t field) {
    return { UsbDeviceSim::pid_byte(pid),
             (uint8_t)(field & 0xFF),
             (uint8_t)((UsbDeviceSim::token_crc5(field) << 3) | (field >> 8)) };
}

// One transaction against the device model, synchronously: token,
// the data stage where the direction has one, and the handshake —
// classified into XFER_STATUS exactly as usbhc_txn does.
static void usb_run_txn() {
    // No device on the port: nothing answers, every transaction ends
    // in the turnaround timeout.
    if (!usb.dev_attached) {
        usb.result = USB_RES_TIMEOUT;
        usb.irq_status |= USB_IRQ_XFER;
        return;
    }

    int pid_sel = usb.token & 3;
    uint32_t devaddr  = (usb.token >> 4) & 0x7F;
    uint32_t endpoint = (usb.token >> 11) & 0xF;
    bool     toggle   = (usb.token >> 16) & 1;
    uint16_t field = (uint16_t)(devaddr | (endpoint << 7));

    // TOKEN.PID 3 is undefined; the RTL's mux falls to IN.
    uint8_t tokpid = pid_sel == 0 ? UsbDeviceSim::PID_SETUP
                   : pid_sel == 1 ? UsbDeviceSim::PID_OUT
                                  : UsbDeviceSim::PID_IN;
    usb.dev.host_packet(usb_token_bytes(tokpid, field));

    if (tokpid != UsbDeviceSim::PID_IN) {
        // SETUP/OUT: the host always transmits the data stage from the
        // buffer; the device answers with a handshake or stays silent.
        uint32_t n = usb.length > USB_BUF_BYTES ? USB_BUF_BYTES : usb.length;
        std::vector<uint8_t> payload(usb.buf, usb.buf + n);
        usb.dev.host_packet(UsbDeviceSim::data_packet(
            toggle ? UsbDeviceSim::PID_DATA1 : UsbDeviceSim::PID_DATA0,
            payload));
        if (!usb.dev.has_response()) {
            usb.result = USB_RES_TIMEOUT;
        } else {
            uint8_t pid = usb.dev.take_response()[0] & 0xF;
            usb.result = pid == UsbDeviceSim::PID_ACK   ? USB_RES_ACK
                       : pid == UsbDeviceSim::PID_NAK   ? USB_RES_NAK
                       : pid == UsbDeviceSim::PID_STALL ? USB_RES_STALL
                       : USB_RES_ERROR;   // e.g. DATAx answering an OUT
        }
    } else {
        // IN: the device answers with a data packet, a handshake, or
        // silence.
        if (!usb.dev.has_response()) {
            usb.result = USB_RES_TIMEOUT;
        } else {
            // rsp is the device's complete response packet, as received
            // off the wire. A handshake is the bare PID byte; a DATAx
            // packet is {PID byte, payload bytes, CRC16 low, CRC16 high}.
            std::vector<uint8_t> rsp = usb.dev.take_response();
            uint8_t pid = rsp[0] & 0xF;
            bool is_data = pid == UsbDeviceSim::PID_DATA0 ||
                           pid == UsbDeviceSim::PID_DATA1;
            if (is_data) {
                // The packet body (CRC bytes included) is stored as it
                // arrives; writes clamp at the buffer bound.
                size_t body = std::min<size_t>(rsp.size() - 1, USB_BUF_BYTES);
                std::copy(rsp.begin() + 1, rsp.begin() + 1 + body, usb.buf);
                usb.rxlen = rsp.size() - 3;
                usb.rxtoggle = pid == UsbDeviceSim::PID_DATA1;
                if (usb.rxlen <= usb.length && usb.rxlen <= USB_BUF_BYTES) {
                    usb.dev.host_packet({UsbDeviceSim::pid_byte(UsbDeviceSim::PID_ACK)});
                    usb.result = USB_RES_ACK;
                } else {
                    usb.result = USB_RES_OVERFLOW;
                }
            } else {
                switch (pid) {
                    case UsbDeviceSim::PID_NAK:
                        usb.result = USB_RES_NAK;
                        break;
                    case UsbDeviceSim::PID_STALL:
                        usb.result = USB_RES_STALL;
                        break;
                    default:
                        usb.result = USB_RES_ERROR;
                        break;
                }
            }
        }
    }

    // DONE is the sticky XFER_DONE source; XFER_STATUS mirrors it.
    usb.irq_status |= USB_IRQ_XFER;
}

// PORT_CTRL write: recipes act on the port view immediately — the
// debounce and reset holds are line timing the ISS does not model
// (software times the ≥10 ms reset hold; completing it early is
// invisible to a correct driver).
static void usb_port_ctrl_write(uint32_t val) {
    bool was_reset = usb.reset_active();
    usb.port_ctrl = val & 0x1F;

    // Power off: there is no port; losing the device is a view change.
    if (!usb.power() && usb.connect) {
        usb.connect = false;
        usb.enabled = false;
        usb.irq_status |= USB_IRQ_PORT;
    }
    // Power on with a device attached: the qualified connect.
    if (usb.power() && usb.dev_attached && !usb.connect) {
        usb.connect = true;
        usb.irq_status |= USB_IRQ_PORT;
    }
    // Reset drive disables the port and returns the device to its
    // power-on protocol state; the completed reset (drive released)
    // enables a connected port and marks the change.
    if (usb.reset_active()) {
        usb.enabled = false;
        if (!was_reset && usb.connect)
            usb.dev.bus_reset();
    }
    if (was_reset && !usb.reset_active() && usb.connect) {
        usb.enabled = true;
        usb.irq_status |= USB_IRQ_PORT;
    }
}

static uint32_t usb_read(uint32_t addr) {
    if (addr & 0x40) {                    // DATA window 0x40..0x7C
        return rd32(&usb.buf[addr & 0x3C]);
    }
    switch (addr & 0x3C) {
        case 0x00:  // CAP: version 1, 64-byte buffer, LS+FS
            return 1 | (USB_BUF_BYTES << 8) | (1u << 16) | (1u << 17);
        case 0x04: return usb.irq_status;
        case 0x08: return usb.irq_enable;
        case 0x0C: {  // PORT_STATUS
            // LINE is the raw {D-, D+} pair: SE0 while the host drives
            // a reset or nothing is attached, full-speed idle J else.
            uint32_t speed = usb.connect ? 2u : 0u;
            uint32_t line = (usb.connect && !usb.reset_active()) ? 1u : 0u;
            return (line << 8) | (speed << 4) |
                   (((usb.port_ctrl >> 3) & 1) << 3) |     // SUSPENDED
                   ((uint32_t)usb.reset_active() << 2) |
                   ((uint32_t)usb.enabled << 1) |
                   (uint32_t)usb.connect;
        }
        case 0x10: return usb.port_ctrl;
        case 0x14: return usb.frame;
        case 0x18: return usb.token;
        case 0x20:  // XFER_STATUS — DONE mirrors the sticky IRQ bit
            return (usb.rxlen << 8) | ((uint32_t)usb.rxtoggle << 4) |
                   (usb.result << 1) | (usb.irq_status & 1);
        default: return 0;   // XFER_CTRL and holes read zero
    }
}

static void usb_write(uint32_t addr, uint32_t data) {
    if (addr & 0x40) {                    // DATA window
        wr32(&usb.buf[addr & 0x3C], data);
        return;
    }
    switch (addr & 0x3C) {
        case 0x04: usb.irq_status &= ~(data & 7); break;   // W1C
        case 0x08: usb.irq_enable = data & 7; break;
        case 0x10: usb_port_ctrl_write(data); break;
        case 0x18: usb.token = data & 0x1FFFF; break;
        case 0x1C:
            usb.length = data & 0x7F;
            if (data & (1u << 16))
                usb_run_txn();
            break;
        // CAP, IRQ_STATUS reads, PORT_STATUS, FRAME, XFER_STATUS: read-only
    }
}

// Called once per ISS instruction: the 1 ms frame marker under RUN.
static void usb_step() {
    if (!usb.run()) return;
    if (++usb.frame_cnt < USB_FRAME_PRESCALE) return;
    usb.frame_cnt = 0;
    usb.frame = (usb.frame + 1) & 0x7FF;
    usb.irq_status |= USB_IRQ_SOF;
    // The marker on the wire: a full-speed SOF token (the ISS device
    // is full-speed; low-speed would get the bare-PID keep-alive).
    if (usb.connect)
        usb.dev.host_packet(usb_token_bytes(UsbDeviceSim::PID_SOF,
                                            (uint16_t)usb.frame));
}

// Physical memory read. Returns bus_fault=true if unmapped.
static uint32_t phys_read(uint32_t addr, int size, bool& bus_fault) {
    bus_fault = false;

    // RAM: from 0x00000000, ram_size bytes
    if (addr < ram_size) {
        if (size==0) return rd8(&ram[addr]);
        if (size==1) return rd16(&ram[addr & ~1u]);
        return rd32(&ram[addr & ~3u]);
    }

    // Autoconfig space: 0xFE000000 (first unconfigured device answers)
    if (const AcfgSlot* s = acfg.cfg_slot(busctl.cfg_en(), addr)) {
        return acfg_config_read(*s, addr);
    }

    // SPI at dynamic base (when configured)
    if (acfg.slot[ACFG_SLOT_SPI].dev_sel(addr)) {
        int reg = (addr >> 2) & 7;
        if (reg == 3) { // DATA — has pop side-effect
            if (spi.fifo_en())
                return spi.rx_pop();
            else
                return spi.rx_data;
        }
        return spi.read_reg(reg);
    }

    // USB host controller at dynamic base
    if (acfg.slot[ACFG_SLOT_USB].dev_sel(addr)) {
        return usb_read(addr & 0x7F);
    }

    // Display adapter at dynamic base
    if (acfg.slot[ACFG_SLOT_DISPLAY].dev_sel(addr)) {
        return display.read(addr, size);
    }

    // Audio output at dynamic base
    if (acfg.slot[ACFG_SLOT_AUDIO].dev_sel(addr)) {
        return audio.read(addr);
    }

    // Ethernet adapter at dynamic base
    if (acfg.slot[ACFG_SLOT_ETH].dev_sel(addr)) {
        return eth.read(addr, size);
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
    if (addr < ram_size) {
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

    // Autoconfig: write to CFG_BASE configures the presented device
    // and advances the chain to the next one
    if (AcfgSlot* s = acfg.cfg_slot(busctl.cfg_en(), addr)) {
        if (((addr >> 2) & 7) == 7) { // CFG_BASE register
            s->base_addr = data;
            s->configured = true;
        }
        return;
    }

    // SPI at dynamic base
    if (acfg.slot[ACFG_SLOT_SPI].dev_sel(addr)) {
        spi_write((addr >> 2) & 7, data);
        return;
    }

    // USB host controller at dynamic base
    if (acfg.slot[ACFG_SLOT_USB].dev_sel(addr)) {
        usb_write(addr & 0x7F, data);
        return;
    }

    // Display adapter at dynamic base
    if (acfg.slot[ACFG_SLOT_DISPLAY].dev_sel(addr)) {
        display.write(addr, data, size);
        return;
    }

    // Audio output at dynamic base
    if (acfg.slot[ACFG_SLOT_AUDIO].dev_sel(addr)) {
        audio.write(addr, data);
        return;
    }

    // Ethernet adapter at dynamic base
    if (acfg.slot[ACFG_SLOT_ETH].dev_sel(addr)) {
        eth.write(addr, data, size);
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

// What TLB_INDEX selects under the presented version: a pinned or a
// main slot, or none (index < 0) for a slot number past its TLB's end.
struct TlbSlot { bool pinned; int index; };
static TlbSlot tlb_selected() {
    uint32_t sel = mmu_version >= 2 ? 0x80000000u : 0x40u;
    uint32_t slot = mmu.tlb_idx & (mmu_version >= 2 ? 0x7FFFFFFFu : 0x3Fu);
    bool pinned = mmu.tlb_idx & sel;
    if (mmu_version < 2 && pinned) slot &= PTLB_ENTRIES - 1;
    uint32_t count = pinned ? PTLB_ENTRIES : (uint32_t)tlb.pte.size();
    return { pinned, slot < count ? (int)slot : -1 };
}

// Version 2's invalidation registers: every valid main-TLB entry the
// written value names goes (TLB_INVAL_VA reads only the VPN's set), and
// the TLB model hears of each as of a software invalidation.
static void tlb_invalidate(int reg, uint32_t val) {
    uint32_t vpn = TLB_VPN_GET_VPN(val), asid = TLB_VPN_GET_ASID(val);
    uint32_t sets = mmu_geometry.sets;
    for (uint32_t i = 0; i < tlb.pte.size(); i++) {
        uint32_t pte = tlb.pte[i], word = tlb.vpn[i];
        if (!(pte & TLB_V)) continue;
        bool global = pte & TLB_G;
        bool hit;
        if (reg == MMU_INVAL_VA)
            hit = i % sets == (vpn & (sets - 1)) && TLB_VPN_GET_VPN(word) == vpn &&
                  (global || TLB_VPN_GET_ASID(word) == asid);
        else if (reg == MMU_INVAL_ASID)
            hit = !global && TLB_VPN_GET_ASID(word) == asid;
        else
            hit = true;
        if (!hit) continue;
        if (tlb_model) tlb_model->real_slot_written(word, pte, pte & ~TLB_V);
        tlb.pte[i] = pte & ~TLB_V;
    }
}

static uint32_t sysreg_read(int dev, int reg) {
    switch (dev) {
    case SYSDEV_MMU:
        switch (reg) {
            case MMU_CFG: return mmu_config_value();
            case MMU_CR: return mmu.cr;
            case MMU_FADDR: return mmu.fault_addr;
            case MMU_FSTAT: return mmu.fault_status;
            case MMU_TLB_VPN: {
                TlbSlot t = tlb_selected();
                if (t.index < 0) return 0;
                return t.pinned ? ptlb.vpn[t.index] : tlb.vpn[t.index];
            }
            case MMU_TLB_PTE: {
                TlbSlot t = tlb_selected();
                if (t.index < 0) return 0;
                return t.pinned ? ptlb.pte[t.index] : tlb.pte[t.index];
            }
            case MMU_TLB_IDX: return mmu.tlb_idx;
            default: return 0;
        }
    case SYSDEV_CPU: return cpuid_read(reg);
    case SYSDEV_L1_DCACHE: case SYSDEV_L1_ICACHE: case SYSDEV_L2_CACHE:
        return cache_read(dev, reg);
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
                // Writing PTE commits VPN + PTE into the selected slot
                TlbSlot t = tlb_selected();
                if (t.index < 0) break;
                if (t.pinned) {
                    ptlb.vpn[t.index] = mmu.tlb_vpn_reg;
                    ptlb.pte[t.index] = val;
                } else {
                    if (tlb_model)
                        tlb_model->real_slot_written(tlb.vpn[t.index], tlb.pte[t.index], val);
                    tlb.vpn[t.index] = mmu.tlb_vpn_reg;
                    tlb.pte[t.index] = val;
                }
                break;
            }
            case MMU_INVAL_VA: case MMU_INVAL_ASID: case MMU_INVAL_ALL:
                if (mmu_version >= 2) tlb_invalidate(reg, val);
                break;
        }
        break;
    case SYSDEV_BUS:
        if (reg == 0) {
            busctl.reg = val;
            // Bus RST unconfigures the chain and resets the SPI; the
            // USBHC core sees only power-on reset (the machine wiring).
            if (busctl.rst()) { acfg.reset(); spi.reset(); display.reset();
    audio.reset(); eth.reset(); }
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
    case SYSDEV_L1_DCACHE: case SYSDEV_L1_ICACHE: case SYSDEV_L2_CACHE:
        cache_write(dev, reg, val);
        break;
    }
}

// ═══════════════════════════════════════════════════════════════
// Exception Entry
// ═══════════════════════════════════════════════════════════════

static bool pc_written;  // Set by exception entry, branches, jumps
static bool insn_trapped;  // The instruction being executed entered an exception

static void exception_entry(int vector) {
    insn_trapped = true;
    if (pair_model) pair_model->boundary();
    if (opstats_enabled) opstats_exception(vector);
    if (pcprof_on) {
        pcprof_vector[vector & 15]++;
        if (vector == VEC_TLB_MISS) pcprof_miss[mmu.fault_addr >> 12]++;
    }
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
// Branch statistics (+branchstats)
// ═══════════════════════════════════════════════════════════════
//
// Tallies executed control transfers by their architectural shape —
// conditional vs. unconditional, forward vs. backward displacement, taken
// vs. not-taken, PC-relative vs. register-indirect. These are functions of
// the instruction stream alone, so the ISS reports them exactly. It models no
// pipeline or predictor: turning these counts into a prediction-recovery or
// flush estimate is a microarchitecture question for an off-line consumer.
static bool branchstats_enabled = false;

struct BranchStats {
    // Conditional PC-relative (Bcc, cond 1..14), by displacement sign + outcome.
    uint64_t bcond_bwd_taken    = 0;  // backward & taken
    uint64_t bcond_bwd_nottaken = 0;  // backward & not-taken
    uint64_t bcond_fwd_taken    = 0;  // forward  & taken
    uint64_t bcond_fwd_nottaken = 0;  // forward  & not-taken
    // Unconditional PC-relative (B with cond=AL, BL): always taken, target known.
    uint64_t buncond            = 0;
    // Register/indirect (JMP/JALR): always taken, target not statically known.
    uint64_t breg               = 0;
};
static BranchStats bstats;

// Record one PC-relative branch outcome. backward = displacement < 0.
static void record_pcrel_branch(bool conditional, bool backward, bool taken) {
    if (!conditional)            bstats.buncond++;
    else if (backward &&  taken) bstats.bcond_bwd_taken++;
    else if (backward && !taken) bstats.bcond_bwd_nottaken++;
    else if (taken)              bstats.bcond_fwd_taken++;
    else                         bstats.bcond_fwd_nottaken++;
}

// ═══════════════════════════════════════════════════════════════
// Instruction-class histogram and MUL/DIV operand census (+opstats)
// ═══════════════════════════════════════════════════════════════
//
// Buckets every executed instruction by class, so a dynamic instruction-count
// breakdown is legible the way the perfctr stall breakdown is. Architectural
// (depends only on the instruction stream), so the ISS reports it exactly; the
// diagnostic buckets are stack traffic (spills/locals), reg-reg moves
// (codegen redundancy), constant/address materialization, and flag-only
// compares (the flag-ISA tax).  Counted over each measurement window and
// reported with it.

struct OpStats {
    uint64_t alu_rr = 0;    // Format R ALU reg-reg, writes a result
    uint64_t alu_imm = 0;   // Format L ALU with immediate
    uint64_t mov_rr = 0;    // Format R MOV Rd,Rs (reg-reg copy)
    uint64_t cmp_test = 0;  // flag-only ops (CMP/TEST, CMP #imm)
    uint64_t imm_mat = 0;   // LLI/LLIS/LUI — constant / address materialization
    uint64_t lui = 0;       // LUI subset (≈ count of 32-bit constants/addresses)
    uint64_t muldiv = 0;    // MUL/MULU/DIV/DIVU
    uint64_t ld_stack = 0;  // load,  base = R14 (spill / local)
    uint64_t ld_other = 0;  // load,  base ≠ R14
    uint64_t st_stack = 0;  // store, base = R14 (spill / local)
    uint64_t st_other = 0;  // store, base ≠ R14
    uint64_t br_cond = 0;   // conditional / unconditional PC-relative branch
    uint64_t call = 0;      // BL + JALR
    uint64_t ret_jmp = 0;   // JMP Rd (return / indirect)
    uint64_t sys_spr = 0;   // RDSPR/WRSPR/RDSYS/WRSYS
    uint64_t other = 0;     // reserved / unclassified
    // Exceptions taken, interrupts included, by vector: each entry reads
    // the vector table once.
    uint64_t exceptions[16] = {};
};
static OpStats opstats;

static void opstats_exception(int vector) { opstats.exceptions[vector & 15]++; }

static void opstats_record(uint32_t insn) {
    int fmt = (insn >> 30) & 3;
    switch (fmt) {
    case 0: {  // Format R
        int  op    = (insn >> 25) & 0x1F;
        bool f_bit = (insn >> 16) & 1;
        if (op <= 11) {
            if      (op == 8) opstats.mov_rr++;     // MOV Rd, Rs
            else if (f_bit)   opstats.cmp_test++;   // CMP / TEST (flag-only)
            else              opstats.alu_rr++;
        } else if (op >= 16 && op <= 19) {
            opstats.muldiv++;
        } else if (op >= 24) {
            opstats.sys_spr++;
        } else {
            opstats.other++;
        }
        break;
    }
    case 1: {  // Format L
        int op = (insn >> 26) & 0xF;
        if      (op == 0 || op == 1) opstats.imm_mat++;
        else if (op == 2)          { opstats.imm_mat++; opstats.lui++; }  // LUI
        else if (op == 5)            opstats.cmp_test++;                  // CMP #imm
        else if (op == 11)           opstats.ret_jmp++;                   // JMP Rd
        else if (op == 12)           opstats.call++;                      // JALR Rd
        else if (op >= 3 && op <= 10) opstats.alu_imm++;
        else                         opstats.other++;
        break;
    }
    case 2: {  // Format M (load / store)
        bool is_load = (insn >> 29) & 1;
        bool stack   = ((insn >> 18) & 0xF) == 14;  // base = R14 (SP)
        if (is_load) { if (stack) opstats.ld_stack++; else opstats.ld_other++; }
        else         { if (stack) opstats.st_stack++; else opstats.st_other++; }
        break;
    }
    case 3: {  // Format B
        if (((insn >> 26) & 0xF) == 15) opstats.call++;   // BL
        else                            opstats.br_cond++;
        break;
    }
    }
}

// MUL and DIV operands.  The divmul unit takes every step whatever its
// operands hold (doc/internals/divmul.md); this counts the steps one
// that stopped at an operand's highest set bit would need.  Lengths are
// those of the magnitudes, which are what the unit iterates on.
static constexpr int DIVMUL_BITS = 32;      // operand width, and a divide's steps at one quotient bit each

// Divides by the aligned steps they need: none, then up to each bound.
static constexpr int DIV_ALIGNED_BUCKETS = 4;
static constexpr int DIV_ALIGNED_BOUNDS[DIV_ALIGNED_BUCKETS] = { 0, 8, 16, DIVMUL_BITS };

struct MulDivCensus {
    uint64_t mul = 0;                   // MUL, MULU
    uint64_t mul_steps_rd = 0;          // steps needed walking Rd, the operand the unit walks
    uint64_t mul_steps_rs = 0;          // ... walking Rs
    uint64_t mul_steps_shorter = 0;     // ... walking the shorter of the two
    uint64_t div = 0;                   // DIV, DIVU that did not trap
    uint64_t div_steps_dividend = 0;    // steps needed past the dividend's leading zeros
    uint64_t div_steps_aligned = 0;     // ... with the dividend aligned to the divisor
    uint64_t div_by_aligned[DIV_ALIGNED_BUCKETS] = {};
};
static MulDivCensus muldiv_census;

static void opstats_clear() {
    opstats = OpStats();
    muldiv_census = MulDivCensus();
}

// The significant bits of an operand as the unit iterates on it: of its
// magnitude when the op is signed.
static int divmul_operand_bits(uint32_t v, bool is_signed) {
    if (is_signed && (v >> 31)) v = 0u - v;
    return v ? DIVMUL_BITS - __builtin_clz(v) : 0;
}

// Steps a multiply needs to walk a multiplier of `bits` significant
// bits, at the multiplier bits the +core= generation retires a step.
static int mul_steps_needed(int bits) {
    int per_step = (int)core_gen->mul_bits_per_step;
    return (bits + per_step - 1) / per_step;
}

// Steps a divide needs aligned to its divisor: the remainder starts
// with the dividend's leading bits that cannot yet reach it.
static int div_steps_aligned(int dividend_bits, int divisor_bits) {
    if (dividend_bits < divisor_bits)
        return 0;
    else
        return dividend_bits - divisor_bits + 1;
}

// One MUL or DIV that did not trap, with the operands it read: `rd_val`
// the multiplier or dividend, `rs_val` the multiplicand or divisor.
static void opstats_muldiv(int op, uint32_t rd_val, uint32_t rs_val) {
    MulDivCensus& m = muldiv_census;
    bool is_signed = op == 16 || op == 18;      // MUL, DIV; MULU and DIVU are 17 and 19
    int rd_bits = divmul_operand_bits(rd_val, is_signed);
    int rs_bits = divmul_operand_bits(rs_val, is_signed);
    if (op <= 17) {
        m.mul++;
        m.mul_steps_rd      += mul_steps_needed(rd_bits);
        m.mul_steps_rs      += mul_steps_needed(rs_bits);
        m.mul_steps_shorter += mul_steps_needed(std::min(rd_bits, rs_bits));
    } else {
        int aligned = div_steps_aligned(rd_bits, rs_bits);
        if (aligned < 0 || aligned > rd_bits) {
            fprintf(stderr, "opstats: %d aligned steps for a dividend of %d bits\n", aligned, rd_bits);
            abort();
        }
        int bucket = 0;
        while (aligned > DIV_ALIGNED_BOUNDS[bucket]) bucket++;
        m.div++;
        m.div_steps_dividend += rd_bits;
        m.div_steps_aligned  += aligned;
        m.div_by_aligned[bucket]++;
    }
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
    bool cacheable;
    int fault = mmu_translate(vaddr, 1 /*read*/, size, false, paddr, cacheable);
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

    if (cache_model) {
        Served served = cache_model->load(paddr, cacheable, supervisor(), cache_enables());
        if (sb_model) sb_model->load(served, paddr);
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
    bool cacheable;
    int fault = mmu_translate(vaddr, 2 /*write*/, size, false, paddr, cacheable);
    if (fault) { exception_entry(fault); took_exception = true; return; }

    bool bus_fault;
    phys_write(paddr, data, size, bus_fault);
    if (!bus_fault && cache_model)
        cache_model->store(paddr, cacheable, supervisor(), cache_enables());
    if (!bus_fault && sb_model)
        sb_model->store(paddr, 1u << size, cacheable && cache_enables().l1d);
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
    bool cacheable;
    int fault = mmu_translate(cpu.pc, 4 /*exec*/, 2 /*word*/, false, paddr, cacheable);
    if (fault) { exception_entry(fault); took_exception = true; return 0; }

    bool bus_fault;
    uint32_t insn = phys_read(paddr, 2, bus_fault);
    if (!bus_fault && cache_model) {
        Served served = cache_model->fetch(paddr, cacheable, supervisor(), cache_enables());
        if (sb_model) sb_model->fetch(served, paddr);
    }
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

// True if the instruction names R15/PC as a GPR destination. PC is not
// register-file writable (only control flow updates it), so such an encoding
// is invalid and traps to VEC_ILLEGAL — matching the hardware decoder.
// Checked before any effects are applied, so the instruction is inert.
static bool writes_r15_gpr_dst(uint32_t insn) {
    int fmt = (insn >> 30) & 3;
    switch (fmt) {
        case 0: {  // Format R
            int op = (insn >> 25) & 0x1F;
            int rd = (insn >> 21) & 0xF;
            bool f = (insn >> 16) & 1;
            if (rd != 15) return false;
            if (op <= 11)              return !f;    // ALU writes rd unless F (CMP/TEST)
            if (op >= 16 && op <= 19)  return true;  // MUL/MULU/DIV/DIVU
            if (op == 24 || op == 31)  return true;  // RDSYS / RDSPR write rd
            return false;                            // WRSYS/WRSPR/SYSCALL/.../reserved
        }
        case 1: {  // Format L
            int op = (insn >> 26) & 0xF;
            int rd = (insn >> 22) & 0xF;
            if (rd != 15) return false;
            switch (op) {  // GPR-writing L ops (not CMPI/TESTI/JMP/JALR)
                case 0: case 1: case 2: case 3: case 4:
                case 6: case 8: case 9: case 10: return true;
                default: return false;
            }
        }
        case 2:    // Format M: a load writes rd (a store does not)
            return ((insn >> 29) & 1) && (((insn >> 22) & 0xF) == 15);
        default:   // Format B: BL writes R13, never rd
            return false;
    }
}

static void execute_one() {
    // Step timer and USB frame clock (one instruction = one prescaler step)
    timer.step();
    usb_step();

    // Check for pending interrupts before fetching.
    // Timer has priority over external (UART) IRQ.
    if ((cpu.sr & SR_I) && !cpu.ei_shadow) {
        // Ahead of the device sources: an injected interrupt aimed at one
        // instruction must not be displaced by a tick that happens to be
        // due on the same cycle.
        if (irq_at_pc_addr != 0 && cpu.pc == irq_at_pc_addr &&
            ++irq_at_pc_arrivals % irq_at_pc_period == 0) {
            irq_at_pc_injected++;
            exception_entry(VEC_EXT_IRQ);
            return;
        }
        if (timer.irq()) {
            exception_entry(VEC_TIMER);
            return;
        }
        // External IRQ: shared wired-OR of UART + SPI + USBHC + Ethernet
        bool spi_irq = (spi.xfer_done && (spi.irq_enable & 1))
                     || (spi.rx_thresh() && (spi.irq_enable & 2))
                     || (spi.tx_thresh() && (spi.irq_enable & 4));
        if (uart.irq() || spi_irq || usb.irq() || eth.irq()) {
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
    const uint32_t insn_pc = cpu.pc;

    bool took_exception;
    uint32_t insn = insn_fetch(took_exception);
    if (took_exception) return;

    // Clear ei_shadow after one instruction post-EI
    bool had_ei_shadow = cpu.ei_shadow;
    cpu.ei_shadow = false;

    pc_written = false;
    insn_trapped = false;
    int fmt = (insn >> 30) & 3;
    if (opstats_enabled) opstats_record(insn);
    if (pcprof_path) {
        // The trigger is a user-mode PC so a profile can start at the
        // workload's own entry point -- a frame or iteration boundary --
        // leaving boot and setup out of the counts.
        if (!pcprof_on && cpu.pc == pcprof_start_pc && !(cpu.sr & SR_S)) {
            pcprof_on = true;
            fprintf(stderr, "\n[PCPROF] counting from PC=0x%08X after %llu instructions\n",
                    cpu.pc, (unsigned long long)cpu.insn_count);
        }
        if (pcprof_on) pcprof_count_insn(cpu.pc, cpu.sr & SR_S);
    }

    // R15/PC as a GPR destination is an invalid encoding → illegal instruction
    // (before any effects, and ahead of any privilege check, so illegal outranks
    // priv as the architecture specifies).
    if (writes_r15_gpr_dst(insn)) {
        exception_entry(VEC_ILLEGAL);
        return;
    }

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

        } else if (op <= 15) {
            // op 12-15: reserved single-cycle ALU slots — illegal
            exception_entry(VEC_ILLEGAL);

        } else if (op <= 19) {
            // Hardware MUL/MULU/DIV/DIVU via the divmul peer unit.
            //   16=MUL (signed)  17=MULU  18=DIV (signed)  19=DIVU
            // Rdh = IR[15:12] is the write-only third operand for the high
            // half of the result (product high for MUL/MULU, remainder for
            // DIV/DIVU). R0 drops writes, so the 2-operand forms discard
            // the high half for free. Divides are always 32/32 — there is
            // no high-half dividend input.
            int      rdh = (insn >> 12) & 0xF;
            uint32_t a   = reg_read(rd);   // multiplier / dividend
            uint32_t b   = reg_read(rs);   // multiplicand / divisor
            uint32_t lo = 0, hi = 0;
            bool fault = false;

            if (op == 16) {                // signed multiply 32x32 -> 64
                int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
                lo = (uint32_t)p;
                hi = (uint32_t)((uint64_t)p >> 32);
            } else if (op == 17) {         // unsigned multiply 32x32 -> 64
                uint64_t p = (uint64_t)a * (uint64_t)b;
                lo = (uint32_t)p;
                hi = (uint32_t)(p >> 32);
            } else if (op == 19) {         // unsigned DIVU, 32/32
                if (b == 0) {
                    fault = true;          // DIV0
                } else {
                    lo = a / b;
                    hi = a % b;
                }
            } else {                       // op == 18: signed DIV, 32/32
                if (b == 0) {
                    fault = true;          // DIV0
                } else if (a == 0x80000000u && b == 0xFFFFFFFFu) {
                    lo = 0x80000000u;      // INT_MIN / -1 (UB): -> INT_MIN, no trap
                    hi = 0;
                } else {
                    lo = (uint32_t)((int32_t)a / (int32_t)b);
                    hi = (uint32_t)((int32_t)a % (int32_t)b);
                }
            }

            if (fault) {
                exception_entry(VEC_ARITH);
            } else {
                if (opstats_enabled) opstats_muldiv(op, a, b);
                // Z/N reflect the low half (Rd value); C/V cleared.
                cpu.sr = (cpu.sr & ~SR_FLAGS)
                       | (((lo >> 31) & 1) ? SR_N : 0)
                       | ((lo == 0) ? SR_Z : 0);
                reg_write(rd,  lo);        // product low / quotient
                reg_write(rdh, hi);        // product high / remainder (R0 -> dropped)
            }

        } else if (op <= 22) {
            // op 20-22: reserved Format R opcodes — illegal
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
                    case SPR_SR:
                        // WRSPR SR is reserved: SR.S/SR.I change via
                        // exception entry / ERET / EI / DI and NZCV via
                        // flag-writing ALU ops, so a direct SR write is
                        // never needed.  Trap as illegal.
                        exception_entry(VEC_ILLEGAL);
                        break;
                    default:
                        if (spr_num >= SPR_SCR0 && spr_num < SPR_SCR0 + N_SCR) {
                            cpu.scr[spr_num - SPR_SCR0] = reg_read(rd);
                        } else {
                            exception_entry(VEC_ILLEGAL);  // undefined SPR number
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
                        } else {
                            exception_entry(VEC_ILLEGAL);  // undefined SPR number
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
            if (bp_model)
                bp_model->transfer(cpu.pc, rd == 13 ? BranchKind::Return : BranchKind::Indirect,
                                   true, rd_val);
            cpu.pc = rd_val;
            pc_written = true;
            if (branchstats_enabled) bstats.breg++;
            break;
        case 12: // JALR Rd (R13 = PC+4, PC = Rd)
            if (bp_model) bp_model->transfer(cpu.pc, BranchKind::IndirectCall, true, rd_val);
            reg_write(13, cpu.pc + 4);
            cpu.pc = rd_val;
            pc_written = true;
            if (branchstats_enabled) bstats.breg++;
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

        // cond 15 = BL (link + always taken); cond 0 = AL (always taken).
        bool taken = (cond == 15) || eval_cond(cond);
        if (cond == 15) reg_write(13, cpu.pc + 4);  // BL links the return address
        if (bp_model)
            bp_model->transfer(cpu.pc, cond == 15 ? BranchKind::Call
                                     : cond == 0  ? BranchKind::Direct
                                     :              BranchKind::Conditional, taken, target);
        if (taken) {
            cpu.pc = target;
            pc_written = true;
        }

        if (branchstats_enabled)
            record_pcrel_branch(/*conditional=*/(cond != 0 && cond != 15),
                                /*backward=*/(offset22 < 0), taken);
        break;
    }
    } // switch(fmt)

    // Advance PC if instruction didn't modify it
    if (!pc_written && !cpu.halted) cpu.pc += 4;

    // Restore ei_shadow if it was set at entry (don't clear prematurely)
    if (had_ei_shadow) cpu.ei_shadow = false;

    if (pair_model) pair_model->insn(insn_pc, insn, pc_written && !insn_trapped, insn_trapped);
    if (loaduse_model) loaduse_model->insn(insn_pc, insn, pc_written && !insn_trapped, insn_trapped);
    cpu.insn_count++;
}

// ═══════════════════════════════════════════════════════════════
// Trace Output
// ═══════════════════════════════════════════════════════════════

static void trace_emit(FILE* fp, uint32_t pc, uint32_t sr, const uint32_t* r) {
    fprintf(fp, "PC=%08x SR=%08x [%c%c%c%c]",
            pc, sr,
            (sr & SR_N) ? 'N' : '-',
            (sr & SR_Z) ? 'Z' : '-',
            (sr & SR_C) ? 'C' : '-',
            (sr & SR_V) ? 'V' : '-');
    for (int i = 1; i <= 14; i++)
        fprintf(fp, " R%d=%08x", i, r[i]);
    fprintf(fp, "\n");
}

static void trace_insn() {
    if (!trace_fp) return;
    if (trace_window > 0) {
        // Windowed: stash a compact record (no per-instruction formatting,
        // which would be far too slow over billions of instructions).
        TraceRec& t = trace_ring[trace_ring_head];
        t.pc = cpu.pc;
        t.sr = cpu.sr;
        for (int r = 1; r <= 14; r++) t.r[r] = cpu.r[r];
        trace_ring_head = (trace_ring_head + 1) % trace_window;
        if (trace_ring_fill < trace_window) trace_ring_fill++;
        return;
    }
    trace_emit(trace_fp, cpu.pc, cpu.sr, cpu.r);
}

// Flush the windowed ring (oldest-first) to the trace file on exit.
static void trace_flush_window() {
    if (!trace_fp || trace_window == 0) return;
    size_t start = (trace_ring_fill < trace_window) ? 0 : trace_ring_head;
    for (size_t i = 0; i < trace_ring_fill; i++) {
        const TraceRec& t = trace_ring[(start + i) % trace_window];
        trace_emit(trace_fp, t.pc, t.sr, t.r);
    }
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
    fprintf(stderr, "Ctrl-A U: Toggle USB device attach (hotplug)\r\n");
    fprintf(stderr, "Ctrl-A D: Dump the display screen as text\r\n");
    fprintf(stderr, "\r\n");
}

// Render the display device's character screen as text: printable
// glyphs pass through, glyph 0 and other non-ASCII draw as ' ' / '.',
// and attributes are not rendered. The cursor cell is marked by
// overlaying '_' when enabled and blank.
static void dump_display() {
    unsigned ccol = display.cursor & 0xFFFF;
    unsigned crow = (display.cursor >> 16) & 0xFFFF;
    fprintf(stderr, "\r\n[DISPLAY] CTRL=0x%x cursor=(%u,%u)%s%s\r\n",
            display.ctrl, ccol, crow,
            (display.ctrl & 1) ? "" : "  ** blanked (ENABLE=0) **",
            (display.ctrl & 0x10)
                ? "  ** framebuffer on screen; cells shown below **" : "");
    fprintf(stderr, "+%.*s+\r\n", 80,
            "----------------------------------------"
            "----------------------------------------");
    for (unsigned r = 0; r < 30; r++) {
        char line[81];
        for (unsigned c = 0; c < 80; c++) {
            uint8_t g = display.cells[r * 80 + c] & 0xFF;
            line[c] = (g >= 0x20 && g < 0x7F) ? (char)g : (g ? '.' : ' ');
        }
        if ((display.ctrl & 2) && crow == r && ccol < 80 && line[ccol] == ' ')
            line[ccol] = '_';
        line[80] = '\0';
        fprintf(stderr, "|%s|\r\n", line);
    }
    fprintf(stderr, "+%.*s+\r\n", 80,
            "----------------------------------------"
            "----------------------------------------");
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
        case 'd':
        case 'D':
            dump_display();
            break;
        case 'u':
        case 'U':
            /*
             * Toggle the USB device's plug state — hotplug.  Unplug
             * drops CONNECT/ENABLED and raises PORT_CHANGE like a
             * real cable pull; replug re-qualifies the connect on a
             * powered port and the device re-enumerates from its
             * power-on state.
             */
            usb.dev_attached = !usb.dev_attached;
            if (!usb.dev_attached && usb.connect) {
                usb.connect = false;
                usb.enabled = false;
                usb.irq_status |= USB_IRQ_PORT;
            } else if (usb.dev_attached && usb.power()) {
                usb.dev.bus_reset();   // a freshly plugged device
                usb.connect = true;
                usb.irq_status |= USB_IRQ_PORT;
            }
            fprintf(stderr, "\r\n[USB] device %s\r\n",
                    usb.dev_attached ? "attached" : "detached");
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
    usb.reset();
    display.reset();
    audio.reset();
    eth.reset();
    cache_devices_reset();
    // An enumerable full-speed device sits attached from power-on,
    // exactly as the RTL program runner presents it.
    usb.dev_attached = true;
    usb.dev.enumerate = true;
    memset(&mmu, 0, sizeof(mmu));
    std::fill(tlb.vpn.begin(), tlb.vpn.end(), 0);
    std::fill(tlb.pte.begin(), tlb.pte.end(), 0);
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════

static double bs_pct(uint64_t a, uint64_t b) {
    return b ? 100.0 * (double)a / (double)b : 0.0;
}

// One window's +opstats report: the instruction-class histogram, then
// the MUL and DIV operand census.
static void opstats_report(FILE* out) {
    OpStats &o = opstats;
    uint64_t total = o.alu_rr + o.alu_imm + o.mov_rr + o.cmp_test + o.imm_mat
                   + o.muldiv + o.ld_stack + o.ld_other + o.st_stack + o.st_other
                   + o.br_cond + o.call + o.ret_jmp + o.sys_spr + o.other;
    if (!total) return;

    auto row = [&](const char *name, uint64_t n, const char *note) {
        fprintf(out, "  %-22s %12llu  %5.1f%%%s\n",
                name, (unsigned long long)n, bs_pct(n, total), note);
    };

    fprintf(out, "-- instruction classes\n");
    fprintf(out, "  total executed         %12llu\n", (unsigned long long)total);
    row("ALU reg-reg",        o.alu_rr,   "");
    row("ALU immediate",      o.alu_imm,  "");
    row("MOV reg-reg",        o.mov_rr,   "   <- redundant-move signal");
    row("CMP/TEST flag-only", o.cmp_test, "   <- flag-ISA tax");
    row("const materialize",  o.imm_mat,  "");
    row("MUL/DIV",            o.muldiv,   "");
    row("load  stack(R14)",   o.ld_stack, "   <- spill/local signal");
    row("load  other",        o.ld_other, "");
    row("store stack(R14)",   o.st_stack, "   <- spill/local signal");
    row("store other",        o.st_other, "");
    row("branch",             o.br_cond,  "");
    row("call (BL/JALR)",     o.call,     "");
    row("return/jmp",         o.ret_jmp,  "");
    if (o.sys_spr) row("sys/spr", o.sys_spr, "");
    if (o.other)   row("other",   o.other,   "");

    fprintf(out, "  ── derived ──\n");
    fprintf(out, "  stack traffic (spill+local)  %5.1f%%   (LUI=%llu, ≈ 32-bit consts/addrs)\n",
            bs_pct(o.ld_stack + o.st_stack, total), (unsigned long long)o.lui);
    fprintf(out, "  control (branch+call+ret)    %5.1f%%\n",
            bs_pct(o.br_cond + o.call + o.ret_jmp, total));
    fprintf(out, "  flag-compare + const-mat     %5.1f%%\n",
            bs_pct(o.cmp_test + o.imm_mat, total));

    static const char* const vector_names[16] = {
        "bus fault", "timer", "TLB miss", "TLB protection", "privilege", "syscall", "break",
        "illegal", "alignment", "external IRQ", "arithmetic", "vector 11", "vector 12",
        "vector 13", "vector 14", "vector 15",
    };
    uint64_t exceptions = 0;
    for (uint64_t e : o.exceptions) exceptions += e;
    fprintf(out, "-- exceptions %llu, %.3f per 1000 instructions\n",
            (unsigned long long)exceptions, 1000.0 * exceptions / total);
    for (int v = 0; v < 16; v++)
        if (o.exceptions[v])
            fprintf(out, "  %-22s %12llu  %8.3f per 1000\n", vector_names[v],
                    (unsigned long long)o.exceptions[v], 1000.0 * o.exceptions[v] / total);

    const MulDivCensus& m = muldiv_census;
    auto n = [](uint64_t v) { return (unsigned long long)v; };
    auto each = [](uint64_t steps, uint64_t ops) { return ops ? (double)steps / (double)ops : 0.0; };
    fprintf(out, "-- MUL and DIV operands\n");
    fprintf(out, "  multiplies %llu, of %u steps each; needed walking Rd %llu (%.2f), Rs %llu (%.2f),"
                 " the shorter %llu (%.2f)\n",
            n(m.mul), DIVMUL_BITS / core_gen->mul_bits_per_step,
            n(m.mul_steps_rd), each(m.mul_steps_rd, m.mul), n(m.mul_steps_rs), each(m.mul_steps_rs, m.mul),
            n(m.mul_steps_shorter), each(m.mul_steps_shorter, m.mul));
    fprintf(out, "  divides %llu, of %d steps each; needed past the dividend's leading zeros %llu (%.2f),"
                 " aligned to the divisor %llu (%.2f)\n",
            n(m.div), DIVMUL_BITS, n(m.div_steps_dividend), each(m.div_steps_dividend, m.div),
            n(m.div_steps_aligned), each(m.div_steps_aligned, m.div));
    fprintf(out, "  divides by steps needed aligned: none %llu", n(m.div_by_aligned[0]));
    for (int i = 1; i < DIV_ALIGNED_BUCKETS; i++)
        fprintf(out, ", %d-%d %llu", DIV_ALIGNED_BOUNDS[i - 1] + 1, DIV_ALIGNED_BOUNDS[i],
                n(m.div_by_aligned[i]));
    fprintf(out, "\n");
}

// Dump the +branchstats histogram to stderr. Raw architectural counts only —
// no predictor or pipeline model (see the struct header).
static void print_branch_stats() {
    if (!branchstats_enabled) return;

    uint64_t bcond_taken    = bstats.bcond_bwd_taken + bstats.bcond_fwd_taken;
    uint64_t bcond_nottaken = bstats.bcond_bwd_nottaken + bstats.bcond_fwd_nottaken;
    uint64_t bcond_total    = bcond_taken + bcond_nottaken;
    uint64_t total_taken    = bcond_taken + bstats.buncond + bstats.breg;
    uint64_t total_branches = bcond_total + bstats.buncond + bstats.breg;

    fprintf(stderr, "\n── branch statistics (+branchstats) ──\n");
    fprintf(stderr, "  instructions retired  %llu\n",
            (unsigned long long)cpu.insn_count);
    fprintf(stderr, "  total branches        %llu  (%.1f%% of insns)\n",
            (unsigned long long)total_branches, bs_pct(total_branches, cpu.insn_count));
    fprintf(stderr, "  total taken           %llu  (%.1f%% of branches)\n",
            (unsigned long long)total_taken, bs_pct(total_taken, total_branches));
    fprintf(stderr, "  cond backward  T/NT   %llu / %llu\n",
            (unsigned long long)bstats.bcond_bwd_taken,
            (unsigned long long)bstats.bcond_bwd_nottaken);
    fprintf(stderr, "  cond forward   T/NT   %llu / %llu\n",
            (unsigned long long)bstats.bcond_fwd_taken,
            (unsigned long long)bstats.bcond_fwd_nottaken);
    fprintf(stderr, "  uncond PC-rel (B/BL)  %llu\n",
            (unsigned long long)bstats.buncond);
    fprintf(stderr, "  register/indirect     %llu\n",
            (unsigned long long)bstats.breg);
}

// The byte count a +ram= argument names ("512M", "128m", "32768K",
// "1G", or plain bytes), or 0 if it names no size RAM can have. RAM
// starts at 0 and the ROM probes it a page at a time, needing the two
// pages the hardware-to-ROM contract promises it (doc/system/bus.md);
// it has to end before config space at ACFG_BASE.
static size_t parse_ram_size(const char* text) {
    char *endp;
    unsigned long long val = strtoull(text, &endp, 10);

    if (strlen(endp) > 1)
        return 0;

    unsigned long long mult;

    switch (tolower(*endp)) {
        case '\0':
            mult = 1;
            break;
        case 'k':
            mult = 1024;
            break;
        case 'm':
            mult = 1024 * 1024;
            break;
        case 'g':
            mult = 1024 * 1024 * 1024;
            break;
        default:
            return 0;
    }

    // Bounded before the multiply, which then cannot wrap; this also
    // rejects a count strtoull saturated.
    if (val > ACFG_BASE / mult)
        return 0;

    val *= mult;

    if (val < 2 * PAGE_BYTES || val % PAGE_BYTES != 0)
        return 0;

    return (size_t)val;
}

int main(int argc, char** argv) {
    const char* hex_path = nullptr;
    const char* sd_path = nullptr;
    const char* usbdev_spec = nullptr;
    const char* trace_path = nullptr;
    const char* audio_path = nullptr;
    const char* core_name = DEFAULT_CORE_GENERATION;
    const char* mmu_spec = nullptr;
    const char* cachemodel_spec = nullptr;
    const char* tlbmodel_spec = nullptr;
    const char* sbmodel_spec = nullptr;
    double sbmodel_cpi = 1;
    const char* bpmodel_spec = nullptr;
    const char* pairmodel_spec = nullptr;
    const char* loadusemodel_spec = nullptr;
    const char* model_report_path = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "+ram=", 5) == 0) {
            ram_size = parse_ram_size(argv[i] + 5);
            if (ram_size == 0) {
                fprintf(stderr, "penumbra-iss: bad +ram= size '%s'\n", argv[i] + 5);
                return 1;
            }
        }
        else if (strncmp(argv[i], "+sdcard=", 8) == 0) sd_path = argv[i] + 8;
        else if (strncmp(argv[i], "+usbdev=", 8) == 0) usbdev_spec = argv[i] + 8;
        else if (strncmp(argv[i], "+trace=", 7) == 0) trace_path = argv[i] + 7;
        else if (strncmp(argv[i], "+audiofile=", 11) == 0) audio_path = argv[i] + 11;
        else if (strncmp(argv[i], "+audio_status=", 14) == 0) {
            if (!audio.set_stage(argv[i] + 14)) {
                fprintf(stderr, "penumbra-iss: bad +audio_status= '%s' "
                        "(busy, busy:N with N >= 1, fault, or fault:CODE "
                        "with CODE 0-255)\n", argv[i] + 14);
                return 1;
            }
        }
        else if (strncmp(argv[i], "+eth=", 5) == 0) {
            if (!eth.configure(argv[i] + 5)) {
                fprintf(stderr, "penumbra-iss: bad +eth= '%s' "
                        "(loop|tap[:name][,mdio][,rx=N][,tx=N][,speed=10|100|1000], "
                        "N a power of two, tx at most %u)\n", argv[i] + 5,
                        (ETH_RX_APERTURE - ETH_TX_APERTURE) / ETH_SLOT_SIZE);
                return 1;
            }
        }
        else if (strncmp(argv[i], "+max-insn=", 10) == 0) max_insns = strtoull(argv[i] + 10, nullptr, 0);
        else if (strcmp(argv[i], "+raw") == 0) full_raw = true;
        else if (strcmp(argv[i], "+hosted") == 0) hosted_mode = true;
        else if (strcmp(argv[i], "+quiet") == 0) quiet_mode = true;
        else if (strcmp(argv[i], "+trap-pc0") == 0) trap_user_pc_zero = true;
        else if (strcmp(argv[i], "+halt-on-break") == 0) halt_on_break = true;
        else if (strcmp(argv[i], "+branchstats") == 0) branchstats_enabled = true;
        else if (strcmp(argv[i], "+opstats") == 0) opstats_enabled = true;
        else if (strncmp(argv[i], "+pcprof=", 8) == 0) pcprof_path = argv[i] + 8;
        else if (strncmp(argv[i], "+pcprof_start=", 14) == 0)
            pcprof_start_pc = (uint32_t)strtoull(argv[i] + 14, nullptr, 0);
        else if (strncmp(argv[i], "+halt_on=", 9) == 0) {
            halt_on_str = argv[i] + 9;
            halt_on_len = strlen(halt_on_str);
            if (halt_on_len > sizeof(halt_on_buf)) halt_on_len = sizeof(halt_on_buf);
        }
        else if (strncmp(argv[i], "+irq_at_pc=", 11) == 0) {
            char *rest = nullptr;
            irq_at_pc_addr = (uint32_t)strtoull(argv[i] + 11, &rest, 0);
            if (rest && *rest == ',')
                irq_at_pc_period = strtoull(rest + 1, nullptr, 0);
            if (irq_at_pc_period == 0) irq_at_pc_period = 1;
        }
        else if (strncmp(argv[i], "+trace_window=", 14) == 0)
            trace_window = (size_t)strtoull(argv[i] + 14, nullptr, 0);
        else if (strncmp(argv[i], "+core=", 6) == 0) core_name = argv[i] + 6;
        else if (strncmp(argv[i], "+mmu=", 5) == 0) mmu_spec = argv[i] + 5;
        else if (strcmp(argv[i], "+cachemodel") == 0) cachemodel_spec = "";
        else if (strncmp(argv[i], "+cachemodel=", 12) == 0) cachemodel_spec = argv[i] + 12;
        else if (strcmp(argv[i], "+tlbmodel") == 0) tlbmodel_spec = "";
        else if (strncmp(argv[i], "+tlbmodel=", 10) == 0) tlbmodel_spec = argv[i] + 10;
        else if (strcmp(argv[i], "+sbmodel") == 0) sbmodel_spec = "";
        else if (strncmp(argv[i], "+sbmodel=", 9) == 0) sbmodel_spec = argv[i] + 9;
        else if (strncmp(argv[i], "+sbmodel_cpi=", 13) == 0) sbmodel_cpi = atof(argv[i] + 13);
        else if (strcmp(argv[i], "+bpmodel") == 0) bpmodel_spec = "";
        else if (strncmp(argv[i], "+bpmodel=", 9) == 0) bpmodel_spec = argv[i] + 9;
        else if (strcmp(argv[i], "+pairmodel") == 0) pairmodel_spec = "";
        else if (strncmp(argv[i], "+pairmodel=", 11) == 0) pairmodel_spec = argv[i] + 11;
        else if (strcmp(argv[i], "+loadusemodel") == 0) loadusemodel_spec = "";
        else if (strncmp(argv[i], "+loadusemodel=", 14) == 0) loadusemodel_spec = argv[i] + 14;
        else if (strncmp(argv[i], "+model_start=", 13) == 0)
            model_window.start_pc = (uint32_t)strtoull(argv[i] + 13, nullptr, 0);
        else if (strncmp(argv[i], "+model_stop=", 12) == 0)
            model_window.stop_pc = (uint32_t)strtoull(argv[i] + 12, nullptr, 0);
        else if (strncmp(argv[i], "+model_report=", 14) == 0) model_report_path = argv[i] + 14;
        else hex_path = argv[i];
    }

    if (!hex_path) {
        fprintf(stderr, "Usage: penumbra-iss [program.hex] [+ram=size] [+sdcard=path] [+usbdev=type:config] [+eth=loop|tap[:name][,mdio][,rx=N][,tx=N][,speed=MBIT]] [+audiofile=path] [+audio_status=busy[:N]|fault[:CODE]] [+trace=path] [+trace_window=N] [+halt_on=str] [+raw] [+hosted] [+quiet] [+max-insn=N] [+trap-pc0] [+halt-on-break] [+branchstats] [+opstats] [+pcprof=path [+pcprof_start=addr]] [+irq_at_pc=addr[,period]] [+core=generation] [+mmu=ver[:SETSxWAYS]] [+cachemodel[=list]] [+tlbmodel[=list]] [+sbmodel[=list] [+sbmodel_cpi=N]] [+bpmodel[=list]] [+pairmodel[=list]] [+loadusemodel[=list]] [+model_start=addr] [+model_stop=addr] [+model_report=path]\n");
        return 1;
    }

    core_gen = find_core_generation(core_name);
    if (!core_gen) {
        fprintf(stderr, "unknown +core generation '%s' (available:", core_name);
        for (const CoreGeneration& g : CORE_GENERATIONS) fprintf(stderr, " %s", g.name);
        fprintf(stderr, ")\n");
        return 1;
    }
    // The MMU the generation is built with, unless +mmu= names another:
    // VER, or VER:SETSxWAYS.  Each version reports only the geometries
    // its bounds reach (doc/system/mmu.md, "Interface Versions").
    mmu_version = core_gen->mmu_version;
    mmu_geometry = core_gen->tlb;
    if (mmu_spec) {
        unsigned ver = 0, sets = mmu_geometry.sets, ways = mmu_geometry.ways;
        char extra = 0;
        int n = sscanf(mmu_spec, "%u:%ux%u%c", &ver, &sets, &ways, &extra);
        if (n != 1 && n != 3) {
            fprintf(stderr, "+mmu=%s: expected VER or VER:SETSxWAYS\n", mmu_spec);
            return 1;
        }
        mmu_version = ver;
        mmu_geometry = { sets, ways };
    }
    uint32_t max_sets = mmu_version >= 2 ? 1u << 20 : 32, max_ways = mmu_version >= 2 ? 31 : 2;
    if (mmu_version < 1 || mmu_version > 2 || !mmu_geometry.sets ||
        (mmu_geometry.sets & (mmu_geometry.sets - 1)) || mmu_geometry.sets > max_sets ||
        !mmu_geometry.ways || mmu_geometry.ways > max_ways) {
        fprintf(stderr, "MMU version %u cannot report a %ux%u main TLB\n",
                mmu_version, mmu_geometry.sets, mmu_geometry.ways);
        return 1;
    }
    tlb.vpn.assign(mmu_geometry.sets * mmu_geometry.ways, 0);
    tlb.pte.assign(mmu_geometry.sets * mmu_geometry.ways, 0);

    if (cachemodel_spec) {
        static CacheModel model;
        std::string err;
        if (!CacheModel::parse(cachemodel_spec, *core_gen, PAGE_BYTES, model, err)) {
            fprintf(stderr, "+cachemodel: %s\n", err.c_str());
            return 1;
        }
        cache_model = &model;
    }
    if (tlbmodel_spec) {
        static TlbModel model;
        std::string err;
        CoreGeneration presented = *core_gen;
        presented.tlb = mmu_geometry;
        if (!TlbModel::parse(tlbmodel_spec, presented, model, err)) {
            fprintf(stderr, "+tlbmodel: %s\n", err.c_str());
            return 1;
        }
        tlb_model = &model;
    }
    if (sbmodel_spec) {
        if (!cache_model) {
            fprintf(stderr, "+sbmodel needs +cachemodel: it takes its fills from the first hierarchy\n");
            return 1;
        }
        if (!(sbmodel_cpi > 0)) {
            fprintf(stderr, "+sbmodel_cpi: must be a positive number of cycles\n");
            return 1;
        }
        static SbModel model;
        std::string err;
        if (!SbModel::parse(sbmodel_spec, sbmodel_cpi, model, err)) {
            fprintf(stderr, "+sbmodel: %s\n", err.c_str());
            return 1;
        }
        sb_model = &model;
    }
    if (bpmodel_spec) {
        static BpModel model;
        std::string err;
        if (!BpModel::parse(bpmodel_spec, model, err)) {
            fprintf(stderr, "+bpmodel: %s\n", err.c_str());
            return 1;
        }
        bp_model = &model;
    }
    if (pairmodel_spec) {
        static PairModel model;
        std::string err;
        if (!PairModel::parse(pairmodel_spec, model, err)) {
            fprintf(stderr, "+pairmodel: %s\n", err.c_str());
            return 1;
        }
        pair_model = &model;
    }
    if (loadusemodel_spec) {
        static LoadUseModel model;
        std::string err;
        if (!LoadUseModel::parse(loadusemodel_spec, model, err)) {
            fprintf(stderr, "+loadusemodel: %s\n", err.c_str());
            return 1;
        }
        loaduse_model = &model;
    }
    if (windows_active()) {
        model_window.out = stderr;
        if (model_report_path && !(model_window.out = fopen(model_report_path, "w"))) {
            fprintf(stderr, "+model_report: cannot open '%s'\n", model_report_path);
            return 1;
        }
        signal(SIGUSR1, model_window_on_signal);
    }

    if (pcprof_path) {
        // One pointer per 4 KB of address space; blocks are allocated as
        // the run reaches them.
        pcprof_pages.assign(1u << 20, nullptr);
        pcprof_on = (pcprof_start_pc == 0);
    }

    // Initialize
    // calloc: a block this size comes straight from the kernel as
    // zero pages, so only the RAM the guest touches costs host memory.
    ram = static_cast<uint8_t*>(calloc(ram_size, 1));
    if (!ram) {
        fprintf(stderr, "penumbra-iss: cannot allocate %zu bytes of RAM\n", ram_size);
        return 1;
    }
    memset(rom, 0, sizeof(rom));
    cpu_reset();

    if (hosted_mode) {
        if (!load_hex(hex_path, ram, ram_size, 0x1000)) return 1;
        cpu.pc = 0x1000;
    } else {
        if (!load_hex(hex_path, rom, ROM_SIZE)) return 1;
    }

    SdCardSim sd(sd_path);
    sd_card = sd.is_present() ? &sd : nullptr;
    if (sd_card) fprintf(stderr, "[SD] card emulation active\n");
    if (!eth.attach()) return 1;

    // The attached USB device's personality: "+usbdev=<type>:<config>"
    // selects a device function; without one the port carries the bare
    // enumerable device.  One port, one device.
    static UsbMassStorageSim usb_msc;
    if (usbdev_spec) {
        if (strncmp(usbdev_spec, "disk:", 5) == 0) {
            if (!usb_msc.attach(usbdev_spec + 5)) return 1;
            usb.dev.set_function(&usb_msc);
            usb.dev.die_at_config = usb_msc.die_config_;
        } else {
            fprintf(stderr,
                "unknown +usbdev type '%s' (available: disk:<image>)\n",
                usbdev_spec);
            return 1;
        }
    }

    if (audio_path) {
        audio.capture = fopen(audio_path, "wb");
        if (audio.capture) fprintf(stderr, "[AUDIO] capturing frames to '%s'\n", audio_path);
        else fprintf(stderr, "[AUDIO] cannot open '%s'\n", audio_path);
    }

    if (trace_path) {
        trace_fp = fopen(trace_path, "w");
        if (trace_fp) fprintf(stderr, "[TRACE] writing to '%s'\n", trace_path);
        else fprintf(stderr, "[TRACE] cannot open '%s'\n", trace_path);
    }

    if (trace_window > 0) {
        if (trace_fp) {
            trace_ring.resize(trace_window);
            fprintf(stderr, "[TRACE] windowed: keeping last %zu instructions\n", trace_window);
        } else {
            fprintf(stderr, "[TRACE] +trace_window ignored (needs +trace=path)\n");
            trace_window = 0;
        }
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

    if (windows_active() && !model_window.start_pc) model_window_open("reset");

    // Main loop
    while (running && !cpu.halted) {
        if (windows_active()) model_window_step();
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

        // +halt_on: the marker just finished printing — stop here so the
        // (windowed) trace ends right at it.
        if (halt_on_hit) {
            fprintf(stderr, "\n[HALT_ON] matched \"%s\" after %lu instructions, PC=0x%08X\n",
                    halt_on_str, (unsigned long)cpu.insn_count, cpu.pc);
            cpu.halted = true;
            break;
        }

        // Poll stdin periodically for UART RX
        if ((cpu.insn_count & 0xFF) == 0) poll_uart_rx();
        // And the host's network for frames, a little less often
        if ((cpu.insn_count & (ETH_POLL_INSNS - 1)) == 0) eth.poll_host();
    }

    // Restore terminal before printing exit summary so \n works normally
    restore_term();

    if (windows_active() && model_window.open) model_window_close("exit");
    print_branch_stats();  // no-op unless +branchstats
    if (irq_at_pc_addr != 0)
        fprintf(stderr, "[IRQ_AT_PC] 0x%08X reached %llu times, injected %llu\n",
                irq_at_pc_addr, (unsigned long long)irq_at_pc_arrivals,
                (unsigned long long)irq_at_pc_injected);
    pcprof_write();        // no-op unless +pcprof

    if (!quiet_mode) {
        if (cpu.halted) {
            fprintf(stderr, "\n[BREAK after %lu instructions, PC=0x%08X]\n",
                    (unsigned long)cpu.insn_count, cpu.pc);
            // Dump watched memory values from raw RAM array
            for (int wi = 0; wi < dbg.watch_count; wi++) {
                uint32_t wpa = dbg.watch_pa[wi];
                if (wpa < ram_size - 3) {
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

    if (trace_fp) {
        trace_flush_window();   // no-op unless +trace_window was set
        fclose(trace_fp);
        fprintf(stderr, "[TRACE] done\n");
    }
    return hosted_mode ? hosted_exit_code : 0;
}
