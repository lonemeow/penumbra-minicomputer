// Verilator testbench for penumbra3_branch_predictor (via
// penumbra3_branch_predictor_test).
//
// Drives IF2's slot, its two L1-I ways and the enqueue as the core does, and
// checks the predictor against its contract
// (doc/internals/penumbra3/branch-prediction.md, "Design"):
//   - a direct transfer in the hit way is predicted taken to the target in
//     its word: B and BL always, Bcc when the direction table calls it
//     taken; the hit way's word decides, whatever the other way holds
//   - nothing is predicted taken without a hit
//   - a return (JMP R13) is predicted taken to the return stack's top;
//     another JMP, and JALR, are not predicted taken
//   - a call (BL, JALR) pushes its next PC as it enqueues and a return pops,
//     a cycle later; a faulting slot moves nothing
//   - training writes the table a cycle after EX sends it, and the counter
//     reaches the slot read with the fetch at IF1, in the prediction

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_branch_predictor_test.h"
#include "verilated.h"

typedef Vpenumbra3_branch_predictor_test Dut;

// ISA constants (penumbra_pkg)
enum { COND_AL = 0, COND_EQ = 1, COND_BL = 15 };
enum { OP_L_JMP = 11, OP_L_JALR = 12 };
enum { OP_R_ADD = 0 };
enum { REG_LR = 13 };

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Dut* d) {
    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
}

// ── Instruction encoders (doc/system/instruction-encoding.md) ────
static uint32_t enc_b(int cond, uint32_t off22) {
    return (3u << 30) | ((cond & 0xF) << 26) | ((off22 & 0x3FFFFF) << 4);
}
static uint32_t enc_l(int op, int rd) {
    return (1u << 30) | ((op & 0xF) << 26) | ((rd & 0xF) << 22);
}
static uint32_t enc_r(int op, int rd, int rs) {
    return ((op & 0x1F) << 25) | ((rd & 0xF) << 21) | ((rs & 0xF) << 17);
}

static const uint32_t P = 0x00401000u;   // the slot's PC
static const uint32_t ALU = enc_r(OP_R_ADD, 1, 2);

static void clear(Dut* d) {
    d->i_fetch_en = 0; d->i_fetch_addr = 0;
    d->i_slot_pc = P; d->i_way_hit = 0;
    d->i_way0_word = ALU; d->i_way1_word = ALU;
    d->i_enq_fire = 0; d->i_enq_fault = 0; d->i_enq_word = ALU; d->i_enq_next_pc = 0;
    d->i_train = 0; d->i_train_pc = 0; d->i_train_counter = 0;
    d->eval();
}

// IF2 holds a slot at P whose lookup hit `hit`, the ways holding w0 and w1.
static void slot(Dut* d, uint32_t hit, uint32_t w0, uint32_t w1) {
    d->i_slot_pc = P; d->i_way_hit = hit;
    d->i_way0_word = w0; d->i_way1_word = w1;
    d->eval();
}

// EX trains pc's counter; the table writes it a cycle later.
static void train(Dut* d, uint32_t pc, uint32_t counter) {
    d->i_train = 1; d->i_train_pc = pc; d->i_train_counter = counter;
    tick(d);
    d->i_train = 0; d->eval();
    tick(d);
}
// IF1 launches a fetch of pc, which reads the table beside the L1-I.
static void fetch(Dut* d, uint32_t pc) {
    d->i_fetch_en = 1; d->i_fetch_addr = pc;
    tick(d);
    d->i_fetch_en = 0; d->eval();
}
// A slot holding `word` enqueues; the return stack moves a cycle later.
static void enqueue(Dut* d, uint32_t word, uint32_t next_pc, int fault = 0) {
    d->i_enq_fire = 1; d->i_enq_word = word; d->i_enq_next_pc = next_pc;
    d->i_enq_fault = fault;
    tick(d);
    d->i_enq_fire = 0; d->i_enq_fault = 0; d->eval();
    tick(d);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;

    clear(d);
    d->i_rst = 1; tick(d); tick(d); d->i_rst = 0;
    d->eval();

    const uint32_t B_40  = enc_b(COND_AL, 0x10);   // B  P + 0x40
    const uint32_t B_80  = enc_b(COND_AL, 0x20);   // B  P + 0x80
    const uint32_t BL_40 = enc_b(COND_BL, 0x10);   // BL P + 0x40
    const uint32_t BEQ_40 = enc_b(COND_EQ, 0x10);  // BEQ P + 0x40
    const uint32_t RET   = enc_l(OP_L_JMP, REG_LR);
    const uint32_t JMP_4 = enc_l(OP_L_JMP, 4);
    const uint32_t JALR_4 = enc_l(OP_L_JALR, 4);

    // ── Direct transfers in the hit way ──────────────────────────
    clear(d);
    slot(d, 0b01, B_40, ALU);
    check("b_way0_taken",  d->o_pred_taken, 1);
    check("b_way0_target", d->o_pred_target, P + 0x40);

    slot(d, 0b10, ALU, B_40);
    check("b_way1_taken",  d->o_pred_taken, 1);
    check("b_way1_target", d->o_pred_target, P + 0x40);

    // The hit way's word decides; the other way's transfer is someone else's.
    slot(d, 0b10, B_40, B_80);
    check("b_hit_way_target", d->o_pred_target, P + 0x80);
    slot(d, 0b10, B_40, ALU);
    check("b_other_way_not_taken", d->o_pred_taken, 0);

    slot(d, 0b01, BL_40, ALU);
    check("bl_taken",  d->o_pred_taken, 1);
    check("bl_target", d->o_pred_target, P + 0x40);

    // ── Nothing is predicted taken without a hit ─────────────────
    slot(d, 0b00, B_40, B_40);
    check("miss_not_taken", d->o_pred_taken, 0);

    // ── A conditional branch goes as the table says ──────────────
    // Each counter trained, then read with the slot's fetch.
    struct { uint32_t counter; int taken; } dir[] = {{0, 0}, {1, 0}, {2, 1}, {3, 1}};
    for (auto& c : dir) {
        char n[64];
        clear(d);
        train(d, P, c.counter);
        fetch(d, P);
        slot(d, 0b01, BEQ_40, ALU);
        snprintf(n, sizeof n, "bcc_%u_taken", c.counter);
        check(n, d->o_pred_taken, c.taken);
        if (c.taken) {
            snprintf(n, sizeof n, "bcc_%u_target", c.counter);
            check(n, d->o_pred_target, P + 0x40);
        }
        snprintf(n, sizeof n, "bcc_%u_counter", c.counter);
        check(n, d->o_pred_counter, c.counter);
    }
    // Another PC's counter is its own: P + 4 keeps whatever it had.
    clear(d);
    train(d, P, 3);
    train(d, P + 4, 0);
    fetch(d, P + 4);
    check("bcc_index_own", d->o_pred_counter, 0);
    fetch(d, P);
    check("bcc_index_p", d->o_pred_counter, 3);

    // ── Returns, calls and the return stack ──────────────────────
    clear(d);
    enqueue(d, BL_40, 0x00402004);           // a call pushes its next PC
    slot(d, 0b01, RET, ALU);
    check("ret_taken",  d->o_pred_taken, 1);
    check("ret_target", d->o_pred_target, 0x00402004);

    enqueue(d, JALR_4, 0x00403008);          // JALR pushes too
    check("jalr_pushed", d->o_pred_target, 0x00403008);

    enqueue(d, RET, 0);                      // a return pops
    check("ret_popped", d->o_pred_target, 0x00402004);

    enqueue(d, BL_40, 0x00404000, 1);        // a faulting call moves nothing
    check("fault_no_push", d->o_pred_target, 0x00402004);

    // JALR and any other JMP take their target from a register.
    slot(d, 0b01, JALR_4, ALU);
    check("jalr_not_taken", d->o_pred_taken, 0);
    slot(d, 0b01, JMP_4, ALU);
    check("jmp_not_taken", d->o_pred_taken, 0);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
