// Penumbra shared constants package
//
// In SystemVerilog, a `package` is like a C header file — it defines
// constants, types, and enums that multiple modules can import.
//
// Usage in a module:
//   import penumbra_pkg::*;    // like C's: using namespace penumbra;
//   Then use REG_ZERO, REG_SP, etc. directly.

/* verilator lint_off UNUSEDPARAM */
package penumbra_pkg;

    // ── Register addresses ──────────────────────────────────────
    localparam logic [3:0] REG_ZERO = 4'd0;   // R0:  hardwired zero
    localparam logic [3:0] REG_LR   = 4'd13;  // R13: link register (by convention)
    localparam logic [3:0] REG_SP   = 4'd14;  // R14: stack pointer (USP/SSP banked)
    localparam logic [3:0] REG_PC   = 4'd15;  // R15: program counter (read-only alias)

    // NOTE: ALU operation encodings are *not* here. They are a
    // core-internal decoder→ALU control contract, not part of the
    // ISA (the R-format `op` field maps through a per-core decode
    // layer, not directly to an ALU opcode — see
    // doc/system/instruction-encoding.md). Each core owns its own
    // encoding: gen1's lives in penumbra1/alu.sv as local OP_*
    // params; gen2's will live in the gen2 core package. This shared
    // package holds only the ISA contract and system/peripheral
    // register maps that both cores and the peripherals depend on.

    // ── Status register bit positions ───────────────────────────
    // Condition flags in [3:0], system bits in [31:30].
    // Bits [29:4] are reserved (read as zero, ignored on write).
    localparam logic [4:0] SR_N = 5'd0;   // Negative flag
    localparam logic [4:0] SR_Z = 5'd1;   // Zero flag
    localparam logic [4:0] SR_C = 5'd2;   // Carry flag
    localparam logic [4:0] SR_V = 5'd3;   // Overflow flag
    localparam logic [4:0] SR_I = 5'd30;  // Interrupt enable (1=enabled)
    localparam logic [4:0] SR_S = 5'd31;  // Supervisor mode (1=supervisor)

    // ── Condition codes (from Format B branch instructions) ─────
    localparam logic [3:0] COND_AL = 4'b0000;  // Always
    localparam logic [3:0] COND_EQ = 4'b0001;  // Equal            (Z=1)
    localparam logic [3:0] COND_NE = 4'b0010;  // Not equal        (Z=0)
    localparam logic [3:0] COND_CS = 4'b0011;  // Carry set / HS   (C=1)
    localparam logic [3:0] COND_CC = 4'b0100;  // Carry clear / LO (C=0)
    localparam logic [3:0] COND_MI = 4'b0101;  // Minus / negative  (N=1)
    localparam logic [3:0] COND_PL = 4'b0110;  // Plus / positive   (N=0)
    localparam logic [3:0] COND_VS = 4'b0111;  // Overflow set      (V=1)
    localparam logic [3:0] COND_VC = 4'b1000;  // Overflow clear    (V=0)
    localparam logic [3:0] COND_HI = 4'b1001;  // Unsigned higher   (C=1 & Z=0)
    localparam logic [3:0] COND_LS = 4'b1010;  // Unsigned lower/same (C=0 | Z=1)
    localparam logic [3:0] COND_GE = 4'b1011;  // Signed >=         (N=V)
    localparam logic [3:0] COND_LT = 4'b1100;  // Signed <          (N!=V)
    localparam logic [3:0] COND_GT = 4'b1101;  // Signed >          (Z=0 & N=V)
    localparam logic [3:0] COND_LE = 4'b1110;  // Signed <=         (Z=1 | N!=V)
    localparam logic [3:0] COND_BL = 4'b1111;  // Branch-and-link   (always, + save LR)

    // ── Instruction format prefixes (IR[31:30]) ─────────────────
    // The 2-bit prefix that selects how the rest of the word decodes.
    // Authoritative encoding: doc/system/instruction-encoding.md.
    localparam logic [1:0] FMT_R = 2'b00;  // register-register ALU + system
    localparam logic [1:0] FMT_L = 2'b01;  // immediate operations
    localparam logic [1:0] FMT_M = 2'b10;  // memory load/store
    localparam logic [1:0] FMT_B = 2'b11;  // branch

    // ── Format R opcodes (IR[29:25], 5-bit) ─────────────────────
    // op[4]=0 is the single-cycle ALU/move region (ADD..SBC, with
    // 01100-01111 reserved); op[4]=1 is the multi-cycle/system region:
    // the divmul peer unit (10000-10011), reserved peer slots
    // (10100-10110), then the system ops (10111-11111).
    localparam logic [4:0] OP_R_ADD     = 5'b00000;
    localparam logic [4:0] OP_R_SUB     = 5'b00001;
    localparam logic [4:0] OP_R_AND     = 5'b00010;
    localparam logic [4:0] OP_R_OR      = 5'b00011;
    localparam logic [4:0] OP_R_XOR     = 5'b00100;
    localparam logic [4:0] OP_R_SHL     = 5'b00101;
    localparam logic [4:0] OP_R_SHR     = 5'b00110;
    localparam logic [4:0] OP_R_SAR     = 5'b00111;
    localparam logic [4:0] OP_R_MOV     = 5'b01000;
    localparam logic [4:0] OP_R_NOT     = 5'b01001;
    localparam logic [4:0] OP_R_ADC     = 5'b01010;
    localparam logic [4:0] OP_R_SBC     = 5'b01011;
    localparam logic [4:0] OP_R_MUL     = 5'b10000;
    localparam logic [4:0] OP_R_MULU    = 5'b10001;
    localparam logic [4:0] OP_R_DIV     = 5'b10010;
    localparam logic [4:0] OP_R_DIVU    = 5'b10011;
    localparam logic [4:0] OP_R_WRSYS   = 5'b10111;
    localparam logic [4:0] OP_R_RDSYS   = 5'b11000;
    localparam logic [4:0] OP_R_SYSCALL = 5'b11001;
    localparam logic [4:0] OP_R_BREAK   = 5'b11010;
    localparam logic [4:0] OP_R_ERET    = 5'b11011;
    localparam logic [4:0] OP_R_EI      = 5'b11100;
    localparam logic [4:0] OP_R_DI      = 5'b11101;
    localparam logic [4:0] OP_R_WRSPR   = 5'b11110;
    localparam logic [4:0] OP_R_RDSPR   = 5'b11111;

    // ── Format L opcodes (IR[29:26], 4-bit) ─────────────────────
    // The "#imm" arithmetic ops carry the same operation as their
    // Format R cousins but a *different* opcode number, so a decoder
    // must remap (not slice) them to an ALU function. 1101-1111 are
    // reserved.
    localparam logic [3:0] OP_L_LLI   = 4'b0000;  // Rd = zero_ext(imm16)
    localparam logic [3:0] OP_L_LLIS  = 4'b0001;  // Rd = sign_ext(imm16)
    localparam logic [3:0] OP_L_LUI   = 4'b0010;  // Rd = Rd | (imm16 << 16)
    localparam logic [3:0] OP_L_ADDI  = 4'b0011;
    localparam logic [3:0] OP_L_SUBI  = 4'b0100;
    localparam logic [3:0] OP_L_CMPI  = 4'b0101;  // SUBI with F (flags only)
    localparam logic [3:0] OP_L_ANDI  = 4'b0110;
    localparam logic [3:0] OP_L_TESTI = 4'b0111;  // ANDI with F (flags only)
    localparam logic [3:0] OP_L_SHLI  = 4'b1000;
    localparam logic [3:0] OP_L_SHRI  = 4'b1001;
    localparam logic [3:0] OP_L_SARI  = 4'b1010;
    localparam logic [3:0] OP_L_JMP   = 4'b1011;  // PC = Rd
    localparam logic [3:0] OP_L_JALR  = 4'b1100;  // R13 = PC+4; PC = Rd

    // ── Memory access size (Format M `sz` field, IR[28:27]) ──────
    // An enum, not a packed bitfield: only these three encodings are
    // defined. 2'b11 is reserved and never produced by a well-formed
    // instruction. Drives sub-word extract (loads) and lane-replicate
    // plus byte-enable (stores).
    localparam logic [1:0] MEM_SZ_BYTE = 2'b00;  // 8-bit
    localparam logic [1:0] MEM_SZ_HALF = 2'b01;  // 16-bit
    localparam logic [1:0] MEM_SZ_WORD = 2'b10;  // 32-bit

    // ── MMU access types (one-hot, matches R/W/X flag positions) ──
    localparam logic [2:0] ACC_READ  = 3'b001;  // bit 0 = R
    localparam logic [2:0] ACC_WRITE = 3'b010;  // bit 1 = W
    localparam logic [2:0] ACC_EXEC  = 3'b100;  // bit 2 = X

    // ── Sysreg device IDs ──────────────────────────────────────
    localparam logic [3:0] SYSDEV_MMU    = 4'd0;   // MMU (TLB, fault regs)
    localparam logic [3:0] SYSDEV_CPU    = 4'd1;   // CPU identity + perfctrs (read-only id)
    localparam logic [3:0] SYSDEV_L1_DCACHE = 4'd2;  // L1 D-cache control
    localparam logic [3:0] SYSDEV_L1_ICACHE = 4'd3;  // L1 I-cache control
    localparam logic [3:0] SYSDEV_BUS       = 4'd4;  // Bus controller (autoconfig, reset)
    // Devices 5–6 reserved
    localparam logic [3:0] SYSDEV_TIMER     = 4'd7;  // Programmable interval timer
    localparam logic [3:0] SYSDEV_MACH      = 4'd8;  // Machine identity (board name, CPU clock freq)
    localparam logic [3:0] SYSDEV_L2_CACHE  = 4'd9;  // L2 unified cache (INFO/CTRL/INVAL)
    // Devices 10–15 reserved for future expansion

    // ── CPU sysreg addresses (dev_id = 1) ───────────────────
    localparam logic [3:0] SYSREG_CPU_ISA           = 4'd0;  // ISA version + feature flags
    localparam logic [3:0] SYSREG_CPU_NAME0         = 4'd1;  // CPU name bytes  0– 3
    localparam logic [3:0] SYSREG_CPU_NAME1         = 4'd2;  // CPU name bytes  4– 7
    localparam logic [3:0] SYSREG_CPU_NAME2         = 4'd3;  // CPU name bytes  8–11
    localparam logic [3:0] SYSREG_CPU_NAME3         = 4'd4;  // CPU name bytes 12–15
    // Performance counters (free-running, 32-bit, reset on system reset)
    localparam logic [3:0] SYSREG_CPU_CYCLES        = 4'd5;  // CPU clock cycles
    localparam logic [3:0] SYSREG_CPU_INSNS_RETIRED = 4'd6;  // Instructions retired
    // Stall-attribution counters: cycles lost to stalls, by cause, mutually
    // exclusive so their sum is the total stall. FUNIT waits on a multi-cycle
    // execution unit (e.g. divmul); IFETCH/LOAD/STORE are memory stalls;
    // HAZARD is a pipeline interlock (a data or structural hazard blocking
    // issue); FLUSH is a front-end redirect or pipeline-fill bubble.
    localparam logic [3:0] SYSREG_CPU_STALL_FUNIT   = 4'd7;  // Stall: multi-cycle execution unit
    localparam logic [3:0] SYSREG_CPU_STALL_IFETCH  = 4'd8;  // Stall: instruction-fetch memory
    localparam logic [3:0] SYSREG_CPU_STALL_LOAD    = 4'd9;  // Stall: data read miss-fill
    localparam logic [3:0] SYSREG_CPU_STALL_STORE   = 4'd10; // Stall: data write round-trip
    localparam logic [3:0] SYSREG_CPU_STALL_HAZARD  = 4'd11; // Stall: pipeline interlock (issue hazard)
    localparam logic [3:0] SYSREG_CPU_STALL_FLUSH   = 4'd12; // Stall: front-end redirect / fill bubble
    // Regs 13–15 reserved for additional CPU performance counters

    // ── MACH sysreg addresses (dev_id = 8) ──────────────────
    localparam logic [3:0] SYSREG_MACH_FEAT   = 4'd0;  // Machine feature flags
    localparam logic [3:0] SYSREG_MACH_NAME0  = 4'd1;  // Machine name bytes  0– 3
    localparam logic [3:0] SYSREG_MACH_NAME1  = 4'd2;  // Machine name bytes  4– 7
    localparam logic [3:0] SYSREG_MACH_NAME2  = 4'd3;  // Machine name bytes  8–11
    localparam logic [3:0] SYSREG_MACH_NAME3  = 4'd4;  // Machine name bytes 12–15
    localparam logic [3:0] SYSREG_MACH_CPU_FREQ = 4'd5; // CPU clock frequency in Hz (board PLL)

    // ── Cache sysreg addresses (shared across DCACHE/ICACHE/L2/L3) ──
    //
    // Every cache device — L1 D/I, L2, future L3 — exposes the same
    // register map.  Software discovers presence by reading INFO; a
    // zero result means the cache is absent (no instantiation, or the
    // device id is unmapped).  Devices that don't need a multi-cycle
    // op (e.g. L1 inval-all is single-cycle) still respond on STATUS
    // and just always return busy=0.
    localparam logic [3:0] SYSREG_CACHE_INFO         = 4'd0;  // R  — geometry; 0 ⇒ absent
    localparam logic [3:0] SYSREG_CACHE_CTRL         = 4'd1;  // RW — [0]=enable (0 at reset)
    localparam logic [3:0] SYSREG_CACHE_INVAL_ALL    = 4'd2;  // W  — any value drops all lines
    localparam logic [3:0] SYSREG_CACHE_INVAL_LINE   = 4'd3;  // W  — physical addr, drops matching line
    localparam logic [3:0] SYSREG_CACHE_FLUSH_ALL    = 4'd4;  // W  — writeback dirty (WB caches only)
    localparam logic [3:0] SYSREG_CACHE_FLUSH_LINE   = 4'd5;  // W  — writeback one line (WB caches only)
    localparam logic [3:0] SYSREG_CACHE_STATUS       = 4'd6;  // R  — [0]=busy (multi-cycle op pending)
    // Regs 7-9 reserved for future control (perfctr CTRL, writeback-buffer status, ...)
    //
    // Performance counters — same layout on every cache device.  32-bit,
    // free-running, reset to 0 on system reset.  Software gets deltas by
    // reading-before / reading-after a measured region (same convention
    // as SYSDEV_CPU's cycles / insns counters).  HIT/MISS classification
    // is determined by the tag array alone — definitions are stable
    // across every WT/WB × WnA/WA × write-invalidate-on-hit combination;
    // only the per-event downstream cost varies with policy.  See
    // doc/system/sysregs.md § "Performance counters" for the full
    // per-configuration interpretation table.
    localparam logic [3:0] SYSREG_CACHE_READ_HITS    = 4'd10; // R  — read accesses that hit a valid line
    localparam logic [3:0] SYSREG_CACHE_READ_MISSES  = 4'd11; // R  — read accesses that missed
    localparam logic [3:0] SYSREG_CACHE_WRITE_HITS   = 4'd12; // R  — write accesses that hit a valid line
    localparam logic [3:0] SYSREG_CACHE_WRITE_MISSES = 4'd13; // R  — write accesses that missed
    // Regs 14-15 reserved for future counters (LINE_FILLS, WRITEBACKS, MISS_STALL_CYCLES, ...)

    // ── Cache INFO register field encoding ────────────────────
    // Unified across all cache devices so a single decoder serves
    // L1 D/I, L2 and any future L3.  Geometry covers the full range
    // we'd plausibly build on FPGA or in discrete logic:
    //   LINE_WORDS  : 1..63 → up to 252-byte lines
    //   NUM_SETS    : 1..32767 → up to 32 Ki sets
    //   NUM_WAYS    : 1..31 → up to 31-way set-associative
    //
    //  [5:0]   LINE_WORDS    (6 bits)
    //  [20:6]  NUM_SETS      (15 bits)
    //  [25:21] NUM_WAYS      (5 bits)
    //  [27:26] ADDRESSING    (2 bits: PIPT/VIPT/VIVT)
    //  [28]    WRITE_BACK    (0=write-through, 1=write-back)
    //  [29]    WRITE_ALLOC   (0=write-no-allocate, 1=write-allocate)
    //  [31:30] reserved
    //
    // "Unified vs split" is not encoded — for L1 a unified design
    // simply leaves the ICACHE (or DCACHE) slot reporting INFO=0;
    // L2+ are always unified in this architecture.
    localparam logic [1:0] CACHE_ADDR_PIPT = 2'd0;
    localparam logic [1:0] CACHE_ADDR_VIPT = 2'd1;
    localparam logic [1:0] CACHE_ADDR_VIVT = 2'd2;

    // ── MMU sysreg addresses (dev_id = 0) ─────────────────────
    localparam logic [3:0] SYSREG_MMU_CR       = 4'd0;  // MMUCR: [0]=M (enable), [15:8]=ASID
    localparam logic [3:0] SYSREG_MMU_FADDR    = 4'd1;  // Faulting virtual address (read-only)
    localparam logic [3:0] SYSREG_MMU_FSTAT    = 4'd2;  // Fault status (read-only)
    localparam logic [3:0] SYSREG_MMU_TLB_VPN  = 4'd3;  // TLB upper: {4'b0, VPN[19:0], ASID[7:0]}
    localparam logic [3:0] SYSREG_MMU_TLB_PTE  = 4'd4;  // TLB lower: {PPN[19:0], SW[3:0], flags[7:0]}
    localparam logic [3:0] SYSREG_MMU_TLB_IDX  = 4'd5;  // TLB slot: bit6=pinned, {way[0], set[4:0]} or {pin_slot[2:0]}

    // ── Bus controller sysreg addresses (dev_id = 4) ─────────────
    localparam logic [3:0] SYSREG_BUS_CTL = 4'd0;  // BUSCTL: [0]=RST (auto-clear), [1]=CFG_EN

    // ── Timer sysreg addresses (dev_id = 7) ─────────────────────
    // 16-bit countdown timer, ticks at a fixed hardware frequency
    // (typically 1 MHz) independent of CPU clock.
    localparam logic [3:0] SYSREG_TM_FREQ   = 4'd0;  // Tick frequency in Hz (read-only, hardwired)
    localparam logic [3:0] SYSREG_TM_CR     = 4'd1;  // Control: [0]=TICK_EN, [1]=IRQ_EN, [2]=AUTOLOAD
    localparam logic [3:0] SYSREG_TM_COUNT  = 4'd2;  // Current 16-bit counter (counts down each tick)
    localparam logic [3:0] SYSREG_TM_RELOAD = 4'd3;  // 16-bit reload value (→COUNT on underflow)
    localparam logic [3:0] SYSREG_TM_STATUS = 4'd4;  // [0]=UDF (underflow), write-1-to-clear

    // ── Autoconfig config space (memory-mapped, active when CFG_EN) ──
    localparam logic [31:0] AUTOCONFIG_BASE = 32'hFE00_0000;
    localparam int          AUTOCONFIG_SIZE = 32;    // 8 word-aligned registers

    // Config space register offsets (word-strided)
    localparam logic [4:0] ACFG_CLASS = 5'h00;  // 0xFE000000: Device class (R)
    localparam logic [4:0] ACFG_SIZE  = 5'h04;  // 0xFE000004: Required size (R)
    localparam logic [4:0] ACFG_ID    = 5'h08;  // 0xFE000008: Device ID (R)
    localparam logic [4:0] ACFG_NAME0 = 5'h0C;  // 0xFE00000C: Name bytes  0-3 (R)
    localparam logic [4:0] ACFG_NAME1 = 5'h10;  // 0xFE000010: Name bytes  4-7 (R)
    localparam logic [4:0] ACFG_NAME2 = 5'h14;  // 0xFE000014: Name bytes  8-11 (R)
    localparam logic [4:0] ACFG_NAME3 = 5'h18;  // 0xFE000018: Name bytes 12-15 (R)
    localparam logic [4:0] ACFG_BASE  = 5'h1C;  // 0xFE00001C: Assigned base (W)

    // Device class codes for ACFG_CLASS
    //
    // The class code identifies the base register protocol the device
    // implements. Generic firmware (boot ROM, stage 1) can use any
    // device whose class it understands without a device-specific driver.
    // Devices with extended features (e.g. DMA-capable SPI) still report
    // the base class and implement the base registers; OS drivers detect
    // extra capabilities via CFG_ID.
    localparam logic [31:0] ACFG_CLASS_UNKNOWN  = 32'd0;  // No standard protocol — needs device-specific driver
    localparam logic [31:0] ACFG_CLASS_MEMORY   = 32'd1;  // Plain memory (RAM/ROM) — no registers, just address space
    localparam logic [31:0] ACFG_CLASS_UART     = 32'd2;  // NS16450-compatible UART register interface
    localparam logic [31:0] ACFG_CLASS_SPI      = 32'd3;  // Penumbra SPI master (DATA/STATUS/CONTROL/CLKDIV)
    localparam logic [31:0] ACFG_CLASS_SD       = 32'd4;  // SD/MMC card slot (SPI register interface, CS0 = card)

    // ── UART register offsets (word-strided within 4 KB page) ────
    // Memory-mapped I/O at 0xFF00_0000. Each 8-bit register
    // occupies a 32-bit word (data in bits [7:0]).
    localparam logic [31:0] UART_BASE = 32'hFF00_0000;

    localparam logic [4:0] UART_RBR = 5'h00;  // Receive buffer (read) / Transmit holding (write)
    localparam logic [4:0] UART_THR = 5'h00;  // (same offset as RBR)
    localparam logic [4:0] UART_IER = 5'h04;  // Interrupt enable
    localparam logic [4:0] UART_IIR = 5'h08;  // Interrupt identification (read)
    localparam logic [4:0] UART_FCR = 5'h08;  // FIFO control (write)
    localparam logic [4:0] UART_LCR = 5'h0C;  // Line control (DLAB = bit 7)
    localparam logic [4:0] UART_MCR = 5'h10;  // Modem control (OUT2 = bit 3)
    localparam logic [4:0] UART_LSR = 5'h14;  // Line status
    localparam logic [4:0] UART_MSR = 5'h18;  // Modem status
    localparam logic [4:0] UART_SCR = 5'h1C;  // Scratch register
    localparam logic [4:0] UART_DLL = 5'h00;  // Divisor latch low (DLAB=1)
    localparam logic [4:0] UART_DLM = 5'h04;  // Divisor latch high (DLAB=1)

    // ── USB host controller (CLASS_USBHC) transaction contract ───
    // Programmer-visible field encodings shared by the MAC and the
    // register tier. Full register map: doc/system/devices/usb-host.md.
    localparam logic [1:0] USBHC_TOKEN_SETUP = 2'd0;  // TOKEN.PID
    localparam logic [1:0] USBHC_TOKEN_OUT   = 2'd1;
    localparam logic [1:0] USBHC_TOKEN_IN    = 2'd2;

    localparam logic [2:0] USBHC_RESULT_ACK      = 3'd0;  // XFER_STATUS.RESULT
    localparam logic [2:0] USBHC_RESULT_NAK      = 3'd1;
    localparam logic [2:0] USBHC_RESULT_STALL    = 3'd2;
    localparam logic [2:0] USBHC_RESULT_TIMEOUT  = 3'd3;
    localparam logic [2:0] USBHC_RESULT_ERROR    = 3'd4;  // CRC / bit-stuff / malformed response
    localparam logic [2:0] USBHC_RESULT_OVERFLOW = 3'd5;  // response past the buffer or LENGTH

    // ── Physical address map (bus base addresses) ──────────────
    // Fixed base addresses for memory-mapped devices.
    // UART_BASE already defined above (32'hFF00_0000).
    localparam logic [31:0] RAM_BASE = 32'h0000_0000;
    localparam logic [31:0] ROM_BASE = 32'hFFFF_0000;

    // ── Special-purpose register (SPR) numbers ───────────────────
    // Used by RDSPR/WRSPR instructions — encoded in IR[15:12]
    localparam logic [3:0] SPR_ESR  = 4'd0;   // Exception SR
    localparam logic [3:0] SPR_EPC  = 4'd1;   // Exception PC
    localparam logic [3:0] SPR_USP  = 4'd2;   // User stack pointer (banked R14)
    localparam logic [3:0] SPR_SR   = 4'd3;   // Current status register
    localparam logic [3:0] SPR_SCR0 = 4'd4;   // Scratch SPR 0 (supervisor scratch)
    localparam logic [3:0] SPR_SCR1 = 4'd5;   // Scratch SPR 1
    localparam logic [3:0] SPR_SCR2 = 4'd6;   // Scratch SPR 2
    localparam logic [3:0] SPR_SCR3 = 4'd7;   // Scratch SPR 3

    // ── Exception vector numbers ────────────────────────────────
    // Vector address = {26'b0, vector_num, 2'b00} (word-aligned table at 0x00)
    localparam logic [3:0] VEC_BUS_FAULT = 4'd0;   // 0x00 — Bus fault (no device at address)
    localparam logic [3:0] VEC_TIMER     = 4'd1;   // 0x04 — Timer interrupt
    localparam logic [3:0] VEC_TLB_MISS  = 4'd2;   // 0x08 — TLB miss (no matching entry)
    localparam logic [3:0] VEC_TLB_PROT  = 4'd3;   // 0x0C — TLB protection fault
    localparam logic [3:0] VEC_PRIV      = 4'd4;   // 0x10 — Privilege violation
    localparam logic [3:0] VEC_SYSCALL   = 4'd5;   // 0x14 — SYSCALL instruction
    localparam logic [3:0] VEC_BREAK     = 4'd6;   // 0x18 — BREAK (software breakpoint)
    localparam logic [3:0] VEC_ILLEGAL   = 4'd7;   // 0x1C — Illegal instruction
    localparam logic [3:0] VEC_ALIGN     = 4'd8;   // 0x20 — Alignment fault
    localparam logic [3:0] VEC_EXT_IRQ   = 4'd9;   // 0x24 — External device interrupt
    localparam logic [3:0] VEC_ARITH     = 4'd10;  // 0x28 — Arithmetic fault (DIV0)

    // ── MMU fault status encoding ─────────────────────────────
    // FAULT_STATUS[3:0] = fault type. FAULT_NONE makes the status
    // self-qualifying: a fault that carries no data address (trap, decode
    // fault) rides the pipeline with FAULT_NONE and leaves the MMU's
    // FAULT_ADDR/FAULT_STATUS untouched at commit — no side-band valid bit.
    localparam logic [3:0] FAULT_NONE     = 4'b0000;
    localparam logic [3:0] FAULT_TLB_MISS = 4'b0001;
    localparam logic [3:0] FAULT_PROT     = 4'b0010;
    localparam logic [3:0] FAULT_ALIGN    = 4'b0011;
    localparam logic [3:0] FAULT_BUS      = 4'b0100;
    // FAULT_STATUS[7:4] = reserved (gap for future fault types)
    // FAULT_STATUS[11:8] = faulting access info
    localparam int FSTAT_R   = 8;   // Faulting access was read
    localparam int FSTAT_W   = 9;   // Faulting access was write
    localparam int FSTAT_X   = 10;  // Faulting access was execute
    localparam int FSTAT_USR = 11;  // Faulting access was user mode

    // Compose the architectural FAULT_STATUS word from its fields. This is the
    // single definition of the layout — fault type in [3:0], the faulting
    // access (one-hot ACC_*) at [FSTAT_R +: 3], user-mode at FSTAT_USR — so the
    // bit positions live here, not re-spelled at every fault site (the MMU
    // stack, the gen2 IF and MEM stages). access_type one-hot maps a read to
    // FSTAT_R, a write to FSTAT_W, an execute to FSTAT_X by construction.
    function automatic logic [31:0] compose_fault_status(
        input logic       user_mode,
        input logic [2:0] access_type,   // ACC_READ / ACC_WRITE / ACC_EXEC
        input logic [3:0] fault_type     // FAULT_*
    );
        logic [31:0] s;
        s               = 32'b0;
        s[3:0]          = fault_type;
        s[FSTAT_R +: 3] = access_type;   // FSTAT_R/W/X are contiguous from bit 8
        s[FSTAT_USR]    = user_mode;
        return s;
    endfunction

    // Address-carrying fault types map 1:1 onto exception vectors; the
    // composed status is the single classification and the vector derives
    // from it (D-side at MEM, I-side at its fault path). Domain: real fault
    // types only — there is no "no vector" encoding in 4 bits, so callers
    // qualify on type != FAULT_NONE before consuming the result.
    function automatic logic [3:0] fault_vec_of(input logic [3:0] fault_type);
        case (fault_type)
            FAULT_TLB_MISS: return VEC_TLB_MISS;
            FAULT_PROT:     return VEC_TLB_PROT;
            FAULT_ALIGN:    return VEC_ALIGN;
            default:        return VEC_BUS_FAULT;  // FAULT_BUS (and out-of-domain)
        endcase
    endfunction

    // ── TLB entry bit positions (64-bit entry) ────────────────
    // Upper word (TLB_VPN sysreg): {4'b0, VPN[19:0], ASID[7:0]}
    // Lower word (TLB_PTE sysreg): {PPN[19:0], SW[7:0], G, U, X, W, R, C, rsvd, V}
    localparam int TLB_V   = 0;   // Valid
    localparam int TLB_C   = 2;   // Cacheable
    localparam int TLB_R   = 3;   // Read permission
    localparam int TLB_W   = 4;   // Write permission
    localparam int TLB_X   = 5;   // Execute permission
    localparam int TLB_U   = 6;   // User-accessible
    localparam int TLB_G   = 7;   // Global (skip ASID match)

endpackage
/* verilator lint_on UNUSEDPARAM */
