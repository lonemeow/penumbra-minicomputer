// uart_host.h — the host end of a board's serial line, for testbenches
// that drive a top at its pins: 8N1 at an exact baud rate, as the
// board's USB-UART bridge runs it, so a top whose bit timing drifts
// fails in simulation the way it would on the board.
//
// Both halves step once per testbench clock. UartHostRx decodes the
// board's TX line, sampling each bit at its ideal middle from the start
// edge, and measures how far each edge inside a frame lands from the
// ideal bit grid. UartHostTx drives the board's RX line from a queue.
#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <string>

class UartHostRx {
public:
    explicit UartHostRx(double bit_cycles) : bit_(bit_cycles) {}

    std::string text;            // bytes received with a good stop bit
    int frame_errors = 0;        // frames whose stop bit was low
    double worst_edge_err = 0;   // as a fraction of the edge's distance from the start
    uint64_t last_activity = 0;  // cycle of the latest line transition
    bool in_frame = false;

    void step(bool line, uint64_t cycle) {
        if (line != last_)
            last_activity = cycle;
        if (!in_frame) {
            if (last_ && !line) {
                in_frame = true;
                start_ = cycle;
                next_ = 0;
                byte_ = 0;
            }
        } else {
            if (line != last_) {
                double bits = double(cycle - start_) / bit_;
                double k = std::round(bits);
                if (k >= 1) {
                    double err = std::fabs(bits - k) / k;
                    if (err > worst_edge_err)
                        worst_edge_err = err;
                }
            }
            if (cycle == sample_at(next_)) {
                if (next_ == 0 && line) {
                    in_frame = false;             // a glitch, not a start bit
                } else if (next_ >= 1 && next_ <= 8) {
                    byte_ |= (uint8_t)line << (next_ - 1);
                } else if (next_ == 9) {
                    if (line)
                        text += (char)byte_;
                    else
                        frame_errors++;
                    in_frame = false;
                }
                next_++;
            }
        }
        last_ = line;
    }

private:
    uint64_t sample_at(int k) const {
        return start_ + (uint64_t)std::llround((k + 0.5) * bit_);
    }

    double bit_;
    uint64_t start_ = 0;
    int next_ = 0;               // next sample: 0 start, 1..8 data, 9 stop
    uint8_t byte_ = 0;
    bool last_ = true;
};

class UartHostTx {
public:
    explicit UartHostTx(double bit_cycles) : bit_(bit_cycles) {}

    // A frame with a low stop bit (stop_bit false) is a framing error.
    void send(uint8_t byte, bool stop_bit = true) { queue_.push_back({byte, stop_bit}); }
    void send(const std::string& s) {
        for (char c : s)
            send((uint8_t)c);
    }
    bool busy() const { return active_ || !queue_.empty(); }

    // The level to drive onto the board's RX pin this cycle.
    bool line(uint64_t cycle) {
        if (!active_) {
            if (queue_.empty())
                return true;
            cur_ = queue_.front();
            queue_.pop_front();
            active_ = true;
            start_ = cycle;
            bit_idx_ = 0;
        }
        while (bit_idx_ < 10 && cycle >= start_ + (uint64_t)std::llround((bit_idx_ + 1) * bit_))
            bit_idx_++;
        if (bit_idx_ >= 10) {
            active_ = false;
            return true;
        }
        if (bit_idx_ == 0)
            return false;
        if (bit_idx_ == 9)
            return cur_.stop_bit;
        return (cur_.byte >> (bit_idx_ - 1)) & 1;
    }

private:
    struct Frame {
        uint8_t byte;
        bool stop_bit;
    };

    double bit_;
    std::deque<Frame> queue_;
    Frame cur_{};
    bool active_ = false;
    uint64_t start_ = 0;
    int bit_idx_ = 0;
};
