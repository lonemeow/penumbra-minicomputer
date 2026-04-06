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
// Usage: penumbra-iss [program.hex] [+sdcard=path] [+trace=path]

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
                if (resp_idx_ >= resp_.size()) { resp_.clear(); resp_idx_ = 0; state_ = S_IDLE; }
                return b;
            }
            state_ = S_IDLE; return 0xFF;
        case S_RECV_DATA:
            wbuf_[write_pos_++] = mosi;
            if (write_pos_ == 515) { flush_write(); return 0x05; }
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
        uint8_t cmd = cmd_[0] & 0x3F;
        switch (cmd) {
        case 0: init_ = false; resp_.push_back(0x01); break;
        case 8: resp_.push_back(0x01);
            for (int i=1;i<5;i++) resp_.push_back(cmd_[i]);
            break;
        case 55: app_cmd_ = true; resp_.push_back(init_?0x00:0x01); break;
        case 41:
            if (!prev_app) { resp_.push_back(0x04); break; }
            init_ = true; resp_.push_back(0x00); break;
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
            resp_.push_back(0x00); write_lba_=cmd_arg(); write_pos_=0;
            state_=S_RECV_DATA; break;
        default: resp_.push_back(0x04); break;
        }
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
    uint8_t cmd_[6]={}; int cmd_pos_=0;
    std::vector<uint8_t> resp_; size_t resp_idx_=0;
    uint8_t wbuf_[515]={}; int write_pos_=0; uint32_t write_lba_=0;
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
enum { VEC_BUS_FAULT=0, VEC_IRQ=1, VEC_TLB_MISS=2, VEC_TLB_PROT=3,
       VEC_PRIV=4, VEC_SYSCALL=5, VEC_BREAK=6, VEC_ILLEGAL=7, VEC_ALIGN=8 };

// Sysreg device IDs
enum { SYSDEV_MMU=0, SYSDEV_SYS=1, SYSDEV_DCACHE=2, SYSDEV_ICACHE=3, SYSDEV_BUS=4 };

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
enum { SPR_ESR=0, SPR_EPC=1, SPR_USP=2, SPR_SR=3 };

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
    bool    thre_int;
    int     tx_busy_count;  // counts down per instruction; 0 = ready
    bool    dlab() const { return lcr & 0x80; }
    bool    tx_ready() const { return tx_busy_count == 0; }
    uint8_t lsr() const { return (uint8_t)((tx_ready()<<6)|(tx_ready()<<5)|rx_ready); }
    uint8_t msr() const { return 0x30; } // CTS+DSR hardwired
    uint8_t iir() const {
        if ((ier&1) && rx_ready) return 0x04;
        if ((ier&2) && thre_int) return 0x02;
        return 0x01;
    }
    bool    irq() const { return ((iir() & 1) == 0) && (mcr & 0x08); }
} uart;

// --- SPI controller ---
static struct {
    uint8_t  rx_data;
    uint8_t  control;   // bit 0 = CS0, bit 1 = CS1
    uint16_t clkdiv;
    bool     done;

    void reset() { rx_data=0xFF; control=0x03; clkdiv=0xFF; done=false; }
    uint32_t read_reg(int reg) const {
        switch (reg) {
            case 0: return rx_data;
            case 1: return done ? 0x02 : 0x00; // STATUS: bit1=DONE, bit0=BUSY
            case 2: return control;
            case 3: return clkdiv;
            default: return 0;
        }
    }
} spi;

static SdCardSim* sd_card = nullptr;

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
static uint32_t sysid_read(int reg) {
    static const uint32_t cpu_name[4] = {0x756E6550,0x6172626D,0x0000312F,0};
    static const uint32_t mach_name[4] = {0x756D6953,0x6F74616C,0x00000072,0};
    switch (reg) {
        case 0: return 1;            // CPU_ISA: version 1
        case 1: return 0;            // MACH_FEAT
        case 2: case 3: case 4: case 5: return cpu_name[reg-2];
        case 6: case 7: case 8: case 9: return mach_name[reg-6];
        default: return 0;
    }
}

// ═══════════════════════════════════════════════════════════════
// Terminal and signal handling
// ═══════════════════════════════════════════════════════════════

static volatile sig_atomic_t running = 1;
static struct termios orig_termios;
static bool term_raw = false;
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
    raw.c_lflag &= ~(ECHO | ICANON);
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
        case 5: return uart.lsr();
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

// SPI register write (triggers SD card exchange on DATA write)
static void spi_write(int reg, uint32_t data) {
    switch (reg) {
        case 0: { // DATA — trigger SPI exchange
            uint8_t mosi = data & 0xFF;
            if (sd_card) {
                sd_card->select(!(spi.control & 1)); // CS0 active-low
                spi.rx_data = sd_card->exchange(mosi);
            } else {
                spi.rx_data = 0xFF;
            }
            spi.done = true;
            break;
        }
        case 2: spi.control = data & 0xFF; break;
        case 3: spi.clkdiv = data & 0xFFFF; break;
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
        int reg = (addr >> 2) & 3;
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
        spi_write((addr >> 2) & 3, data);
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
    case SYSDEV_SYS: return sysid_read(reg);
    case SYSDEV_DCACHE: case SYSDEV_ICACHE:
        if (reg == 0) return (0u << 18) | (4 << 12) | (4 << 6) | 4; // fake geometry
        return 0;
    case SYSDEV_BUS: return (reg == 0) ? busctl.reg : 0;
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
    case SYSDEV_DCACHE: case SYSDEV_ICACHE:
        break; // Cache control: accept and ignore in ISS
    }
}

// ═══════════════════════════════════════════════════════════════
// Exception Entry
// ═══════════════════════════════════════════════════════════════

static bool pc_written;  // Set by exception entry, branches, jumps

static void exception_entry(int vector) {
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
// Instruction Execute
// ═══════════════════════════════════════════════════════════════

static void execute_one() {
    // Check for pending IRQ before fetching
    if ((cpu.sr & SR_I) && !cpu.ei_shadow && uart.irq()) {
        exception_entry(VEC_IRQ);
        return;
    }

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
                exception_entry(VEC_SYSCALL);
                break;
            case 26: // BREAK — halt without vectoring (matches RTL testbench:
                //   o_halted fires at dispatch, before exception entry runs)
                cpu.halted = true;
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
                }
                break;
            case 31: // RDSPR
                switch (spr_num) {
                    case SPR_ESR: reg_write(rd, cpu.esr); break;
                    case SPR_EPC: reg_write(rd, cpu.epc); break;
                    case SPR_USP: reg_write(rd, cpu.usp); break;
                    case SPR_SR:  reg_write(rd, cpu.sr); break;
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

static bool load_hex(const char* path, uint8_t* mem, size_t max_size) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open '%s'\n", path); return false; }
    char line[256];
    size_t addr = 0;
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

static void poll_uart_rx() {
    if (uart.rx_ready) return;
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            uart.rbr = (uint8_t)c;
            uart.rx_ready = true;
        }
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
        else hex_path = argv[i];
    }

    if (!hex_path) {
        fprintf(stderr, "Usage: penumbra-iss [program.hex] [+sdcard=path] [+trace=path]\n");
        return 1;
    }

    // Initialize
    memset(ram, 0, sizeof(ram));
    memset(rom, 0, sizeof(rom));
    cpu_reset();

    if (!load_hex(hex_path, rom, ROM_SIZE)) return 1;

    SdCardSim sd(sd_path);
    sd_card = sd.is_present() ? &sd : nullptr;
    if (sd_card) fprintf(stderr, "[SD] card emulation active\n");

    if (trace_path) {
        trace_fp = fopen(trace_path, "w");
        if (trace_fp) fprintf(stderr, "[TRACE] writing to '%s'\n", trace_path);
        else fprintf(stderr, "[TRACE] cannot open '%s'\n", trace_path);
    }

    signal(SIGINT, sigint_handler);
    raw_mode();

    fprintf(stderr, "── Penumbra ISS (Ctrl-C to exit) ──\n\n");

    // Main loop
    while (running && !cpu.halted) {
        trace_insn();
        execute_one();

        // UART TX busy countdown (models baud delay)
        if (uart.tx_busy_count > 0 && --uart.tx_busy_count == 0)
            uart.thre_int = true;

        // Poll stdin periodically for UART RX
        if ((cpu.insn_count & 0xFF) == 0) poll_uart_rx();
    }

    if (cpu.halted) {
        fprintf(stderr, "\n[BREAK after %lu instructions, PC=0x%08X]\n",
                (unsigned long)cpu.insn_count, cpu.pc);
    } else {
        fprintf(stderr, "\n[Interrupted after %lu instructions, PC=0x%08X]\n",
                (unsigned long)cpu.insn_count, cpu.pc);
    }

    if (trace_fp) { fclose(trace_fp); fprintf(stderr, "[TRACE] done\n"); }
    restore_term();
    return 0;
}
