// i2c_slave.h — a slave on a modelled I2C bus, for testbenches.
//
// Follows the bus one edge at a time, as a real slave does: it shifts a
// bit in on each SCL rise and changes what it drives two steps after
// each SCL fall. It answers one address, records the data bytes written
// to it, and sends the bytes queued for it, 0xFF once they run out.
//
// The testbench owns the bus: it steps the slave once per step of its
// own (tick), and pulls SDA low wherever drive_low is set. One that
// watches the bus's edges itself calls the edge handlers; one that does
// not hands observe() the two lines' levels each step.
//
// A model of a device goes on top through on_boundary, called at every
// START and STOP before the slave moves on: there it reads what was
// written since the last one and queues what a read should return.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

struct I2cSlave {
    enum St { IDLE, RECV, ACK_OUT_WAIT, ACK_OUT, SEND, ACK_IN, ACK_IN_DONE };

    explicit I2cSlave(uint8_t address) : address(address) {}

    uint8_t address;                 // the seven bits it answers
    St      st = IDLE;
    bool    is_addr = false;
    int     bit = 0;
    uint8_t sh = 0;
    bool    ack = false;             // what the ninth bit will carry
    St      after_ack = IDLE;
    uint8_t cur = 0;                 // the byte being sent
    bool    master_acked = false;

    bool    drive_low = false;       // the slave's own pull on SDA
    bool    pending = false;
    int     pending_in = 0;

    bool    ack_data = true;         // acknowledge data bytes written to it
    uint8_t addr_byte = 0;           // the last address byte it answered
    std::vector<uint8_t> written;    // data bytes it acknowledged
    std::deque<uint8_t>  to_send;    // what a read returns, 0xFF once empty
    int     acks = 0, nacks = 0;     // the master's answers to bytes read

    std::function<void()> on_boundary;

    void drive(bool low) { pending = low; pending_in = 2; }
    void tick() { if (pending_in > 0 && --pending_in == 0) drive_low = pending; }

    void load() {
        cur = 0xFF;
        if (!to_send.empty()) { cur = to_send.front(); to_send.pop_front(); }
        bit = 0;
        drive(!(cur & 0x80));
    }

    void on_start() {
        if (on_boundary) on_boundary();
        st = RECV; is_addr = true; bit = 0; sh = 0; drive(false);
    }
    void on_stop() {
        if (on_boundary) on_boundary();
        st = IDLE; drive(false);
    }

    void on_scl_rise(bool sda) {
        if (st == RECV) {
            sh = uint8_t((sh << 1) | sda);
            if (++bit < 8) return;
            if (is_addr) {
                ack = (sh >> 1) == address;
                after_ack = !ack ? IDLE : (sh & 1) ? SEND : RECV;
                if (ack) addr_byte = sh;
            } else {
                ack = ack_data;
                after_ack = ack ? RECV : IDLE;
                if (ack) written.push_back(sh);
            }
            st = ACK_OUT_WAIT;
        } else if (st == ACK_IN) {
            master_acked = !sda;
            (master_acked ? acks : nacks)++;
            st = ACK_IN_DONE;
        }
    }

    void on_scl_fall() {
        switch (st) {
            case ACK_OUT_WAIT: drive(ack); st = ACK_OUT; break;
            case ACK_OUT:
                st = after_ack; is_addr = false; bit = 0; sh = 0;
                if (st == SEND) load(); else drive(false);
                break;
            case SEND:
                if (bit == 7) { drive(false); st = ACK_IN; }
                else { bit++; drive(!((cur << bit) & 0x80)); }
                break;
            case ACK_IN_DONE:
                if (master_acked) { st = SEND; load(); }
                else { st = IDLE; drive(false); }
                break;
            default: break;
        }
    }

    // The lines' levels this step: SDA moving while SCL stays high is a
    // START or a STOP, and SCL's own edges clock the bits.
    void observe(bool scl, bool sda) {
        if (scl && scl_was && sda != sda_was) {
            if (sda) on_stop(); else on_start();
        } else if (scl && !scl_was) {
            on_scl_rise(sda);
        } else if (!scl && scl_was) {
            on_scl_fall();
        }
        scl_was = scl; sda_was = sda;
    }

private:
    bool scl_was = true, sda_was = true;
};
