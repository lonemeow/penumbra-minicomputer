// ps2_device.h — a device on a modelled PS/2 link, for testbenches.
//
// Follows the two lines one step at a time, as a keyboard does. It
// drives the clock for bytes in both directions: a frame of its own
// goes out a bit to each clock pulse, the bit on the data line `setup`
// steps before the clock falls, and a byte from the host comes in on
// eleven pulses it gives once it sees the request to send, each bit
// taken as the clock rises.
//
// It honours the host's hold. It starts a frame only after both lines
// have stood high for `idle_wait` steps, and one that finds the clock
// held low before its stop bit has been clocked is given up and sent
// again from the start. A frame whose stop bit has been clocked counts
// as sent.
//
// The testbench owns the lines: each step it hands step() the two
// levels as the device sees them, and pulls a line low wherever `clk`
// or `data` is false. Times are in the testbench's steps, whatever
// their length: step_hz() says how many make a second.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <vector>

struct Ps2Device {
    enum St { IDLE, SEND_SETUP, SEND_LOW, SEND_HIGH, RECV_DELAY, RECV_LOW, RECV_HIGH };

    // ── Timing, in steps ─────────────────────────────────────
    // Zero until step_hz() sets them: a step has no length of its own.
    int half      = 0;      // the clock stands at each level
    int setup     = 0;      // a bit is on the data line before the clock falls
    int idle_wait = 0;      // both lines stand high before a frame starts
    int tx_delay  = 0;      // from a request to send to the first clock pulse

    // A keyboard's timing, for a testbench that steps `hz` times a
    // second: a 12.5 kHz clock, each bit on the line 5 us ahead of it.
    void step_hz(long hz) {
        auto steps = [hz](long us) { return (int)(us * hz / 1'000'000); };
        half      = steps(40);
        setup     = steps(5);
        idle_wait = steps(50);
        tx_delay  = steps(100);
    }

    // ── Behaviour ────────────────────────────────────────────
    bool present     = true;    // false: nothing is attached, and the lines are left alone
    bool acknowledge = true;    // hold data low on the eleventh pulse of a byte from the host
    int  stall_after = -1;      // stop clocking a byte from the host after this many pulses

    // ── What it asks of the lines: false pulls the line low ──
    bool clk = true, data = true;

    // Frames waiting to go to the host, bit 0 first, and how many of a
    // frame's bits to send (eleven, or fewer to cut one short).
    struct Frame { uint16_t bits; int nbits; };
    std::deque<Frame> to_send;

    std::vector<uint16_t> sent_log;     // the frames it counts as sent, in order
    std::vector<uint8_t>  received;     // the bytes it acknowledged, in order
    int started = 0;                    // frames it began to send
    int cut_short = 0;                  // frames it stopped partway, as told to
    int abandoned = 0;                  // frames given up to the host's hold
    int requests = 0;                   // requests to send it saw
    int stalled = 0;                    // bytes from the host it stopped clocking
    int bad_parity = 0, bad_stop = 0;   // bytes from the host that came malformed

    // Called with each byte from the host once it is acknowledged.
    std::function<void(uint8_t)> on_byte;

    St       st = IDLE;
    int      count = 0;     // steps left in the phase
    int      bit = 0;       // sending: bits whose pulse has passed. Receiving: pulses given
    int      high_run = 0;  // steps both lines have now stood high
    uint16_t in = 0;        // the bits taken from the host, the first in bit 0

    // A byte as a well-formed frame: start, the byte from its least
    // significant bit, odd parity, stop.
    static uint16_t frame_of(uint8_t byte) {
        uint16_t parity = (__builtin_popcount(byte) & 1) ? 0 : 1;
        return (uint16_t)((byte << 1) | (parity << 9) | (1u << 10));
    }

    void send(uint8_t byte)                    { to_send.push_back({frame_of(byte), 11}); }
    void send_bits(uint16_t bits, int n = 11)  { to_send.push_back({bits, n}); }
    void reply(uint8_t byte)                   { to_send.push_front({frame_of(byte), 11}); }

    bool sending() const   { return st == SEND_SETUP || st == SEND_LOW || st == SEND_HIGH; }
    bool receiving() const { return st == RECV_DELAY || st == RECV_LOW || st == RECV_HIGH; }
    bool clocking_in() const { return st == RECV_LOW || st == RECV_HIGH; }

    void step(bool clk_line, bool data_line) {
        if (half <= 0) {
            fprintf(stderr, "Ps2Device: step() before step_hz() gave a step its length\n");
            abort();
        }

        if (!present) {
            clk = data = true;
            st = IDLE;
            high_run = 0;
            return;
        }

        high_run = (clk_line && data_line) ? high_run + 1 : 0;

        switch (st) {
        case IDLE:
            if (clk_line && !data_line) {
                // Data low under a free clock: the host's request.
                requests++;
                st = RECV_DELAY;
                count = tx_delay;
                bit = 0;
                in = 0;
            } else if (!to_send.empty() && high_run >= idle_wait) {
                started++;
                bit = 0;
                put_bit();
                st = SEND_SETUP;
                count = setup;
            }
            break;

        case SEND_SETUP:
            if (!clk_line) { give_up(); break; }
            if (--count <= 0) {
                clk = false;
                st = SEND_LOW;
                count = half;
                // The host takes the bit here, so a whole frame's last
                // bit makes it sent.
                if (bit == 10) sent_log.push_back(to_send.front().bits);
            }
            break;

        case SEND_LOW:
            if (--count <= 0) {
                clk = true;
                bit++;
                Frame f = to_send.front();
                if (bit == f.nbits) {
                    if (f.nbits != 11) cut_short++;
                    to_send.pop_front();
                    data = true;
                    st = IDLE;
                    high_run = 0;
                } else {
                    st = SEND_HIGH;
                    count = half - setup;
                }
            }
            break;

        case SEND_HIGH:
            if (!clk_line) { give_up(); break; }
            if (--count <= 0) { put_bit(); st = SEND_SETUP; count = setup; }
            break;

        case RECV_DELAY:
            if (!clk_line) { st = IDLE; break; }    // the host took the clock back
            if (--count <= 0) { clk = false; st = RECV_LOW; count = half; }
            break;

        case RECV_LOW:
            if (--count <= 0) {
                clk = true;
                bit++;
                if (bit <= 10) in |= (uint16_t)((data_line ? 1 : 0) << (bit - 1));
                if (bit == 11) {
                    clk = data = true;
                    st = IDLE;
                    high_run = 0;
                    break;
                }
                if (stall_after >= 0 && bit >= stall_after) {
                    stalled++;
                    clk = data = true;
                    st = IDLE;
                    high_run = 0;
                    break;
                }
                if (bit == 10 && acknowledge) data = false;
                st = RECV_HIGH;
                count = half;
            }
            break;

        case RECV_HIGH:
            if (!clk_line) { clk = data = true; st = IDLE; high_run = 0; break; }
            if (--count <= 0) {
                clk = false;
                st = RECV_LOW;
                count = half;
                // The eleventh pulse: the host reads the acknowledgement
                // as it falls, and the byte counts from here.
                if (bit == 10) take_byte();
            }
            break;
        }
    }

private:
    void put_bit() { data = (to_send.front().bits >> bit) & 1; }

    void give_up() {
        abandoned++;
        clk = data = true;
        st = IDLE;
        high_run = 0;
    }

    void take_byte() {
        uint8_t byte   = (uint8_t)(in & 0xFF);
        bool    parity = (in >> 8) & 1;
        bool    stop   = (in >> 9) & 1;
        bool    acked  = !data;

        if (((__builtin_popcount(byte) + (parity ? 1 : 0)) & 1) == 0) bad_parity++;
        if (!stop) bad_stop++;
        if (acked) {
            received.push_back(byte);
            if (on_byte) on_byte(byte);
        }
    }
};
