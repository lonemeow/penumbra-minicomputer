// Byte-level USB device responder (simulation)
//
// Models an attached USB device at the packet-byte level: it consumes
// complete host packets as they appear on the wire (PID byte, token field
// or payload, CRC) and produces the device's complete response packet, or
// silence. The transaction pacing — turnaround delays, byte rates, the
// receive window — belongs to the harness that owns the seam; this class
// is pure protocol.
//
// This is the seed of the shared USB device model: the MAC testbench
// drives it against the RTL seam today, and the machine_sim PHY model and
// the ISS's register-level USBHC are meant to reuse it, layering real
// device classes (the HID boot keyboard) on the same packet interface.
// Any interactive input it ever takes must not come from terminal stdin —
// the UART console owns stdin; a keyboard front-end needs its own channel.
//
// Deliberately free of any Verilated or terminal dependency.

#ifndef USB_DEVICE_SIM_H
#define USB_DEVICE_SIM_H

#include <cstdint>
#include <vector>

class UsbDeviceSim {
public:
    // PID nibbles (usb_pkg::usb_pid_e).
    static const uint8_t PID_OUT   = 0x1;
    static const uint8_t PID_IN    = 0x9;
    static const uint8_t PID_SOF   = 0x5;
    static const uint8_t PID_SETUP = 0xD;
    static const uint8_t PID_DATA0 = 0x3;
    static const uint8_t PID_DATA1 = 0xB;
    static const uint8_t PID_ACK   = 0x2;
    static const uint8_t PID_NAK   = 0xA;
    static const uint8_t PID_STALL = 0xE;

    // Scripted response policy, set per test scenario. A real device
    // class would replace this with protocol state (this is where the
    // HID keyboard's report pipeline will eventually sit).
    enum Response {
        RSP_SILENT,   // no answer at all (the host should time out)
        RSP_ACK,      // handshake the data stage
        RSP_NAK,
        RSP_STALL,
        RSP_DATA,     // IN: send in_payload; OUT: protocol-violating DATA0
    };
    Response in_response  = RSP_NAK;   // answer to an IN token
    Response out_response = RSP_ACK;   // answer to an OUT/SETUP data stage
    std::vector<uint8_t> in_payload;   // DATA payload offered to IN
    bool in_toggle = false;            // DATA0/DATA1 of that payload

    // Wire log, checked by the harness.
    struct Token { uint8_t pid; uint8_t addr; uint8_t endp; };
    std::vector<Token>    tokens;        // non-SOF tokens seen
    std::vector<uint16_t> sof_frames;    // SOF frame numbers seen
    int keepalives   = 0;                // bare-PID SOF packets (LS keep-alive)
    int acks_seen    = 0;                // host ACK handshakes received
    int crc_failures = 0;                // any packet whose CRC did not check
    int pid_failures = 0;                // any PID byte whose halves disagreed
    std::vector<uint8_t> out_payload;    // last CRC-good OUT/SETUP payload

    // ── Wire-format helpers (mirror sw/tools/usb_crc.py) ─────────────
    // Public so a harness can anchor-check them against the script's
    // golden values and build expected wire images of its own.

    static uint8_t pid_byte(uint8_t pid) {
        return (uint8_t)(((~pid & 0xf) << 4) | (pid & 0xf));
    }

    static uint32_t reflect(uint32_t value, int width) {
        uint32_t result = 0;
        for (int i = 0; i < width; i++)
            if (value & (1u << i))
                result |= 1u << (width - 1 - i);
        return result;
    }

    // Full on-wire token CRC5 over the 11-bit field: left-shift LFSR
    // remainder fed LSB first, then invert and bit-reverse.
    static uint8_t token_crc5(uint16_t field) {
        uint8_t crc = 0x1f;
        for (int i = 0; i < 11; i++) {
            int feedback = ((crc >> 4) & 1) ^ ((field >> i) & 1);
            crc = (uint8_t)((crc << 1) & 0x1f);
            if (feedback)
                crc ^= 0x05;
        }
        return (uint8_t)reflect(crc ^ 0x1f, 5);
    }

    // Full on-wire data CRC16: remainder, then invert and bit-reverse.
    static uint16_t data_crc16(const std::vector<uint8_t>& data) {
        uint16_t crc = 0xffff;
        for (uint8_t byte : data) {
            for (int i = 0; i < 8; i++) {
                int feedback = ((crc >> 15) & 1) ^ ((byte >> i) & 1);
                crc = (uint16_t)(crc << 1);
                if (feedback)
                    crc ^= 0x8005;
            }
        }
        return (uint16_t)reflect(crc ^ 0xffff, 16);
    }

    // Complete wire image of a data packet: PID byte, payload, CRC16
    // (low byte leads on the wire).
    static std::vector<uint8_t> data_packet(uint8_t pid,
                                            const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> v{ pid_byte(pid) };
        v.insert(v.end(), payload.begin(), payload.end());
        uint16_t crc = data_crc16(payload);
        v.push_back((uint8_t)(crc & 0xff));
        v.push_back((uint8_t)(crc >> 8));
        return v;
    }

    // ── The packet interface ─────────────────────────────────────────

    // A complete host packet, as received off the wire. Any response it
    // provokes is left in the response queue for the harness to pace.
    void host_packet(const std::vector<uint8_t>& bytes) {
        if (bytes.empty())
            return;

        uint8_t pid = bytes[0] & 0xf;
        if (pid_byte(pid) != bytes[0]) {
            pid_failures++;
            return;
        }

        switch (pid) {
        case PID_SETUP:
        case PID_OUT:
        case PID_IN:
        case PID_SOF:
            handle_token(pid, bytes);
            break;
        case PID_DATA0:
        case PID_DATA1:
            handle_data(pid, bytes);
            break;
        case PID_ACK:
            acks_seen++;
            break;
        default:
            break;
        }
    }

    bool has_response() const { return response_pending_; }

    std::vector<uint8_t> take_response() {
        response_pending_ = false;
        return response_;
    }

private:
    bool data_stage_expected_ = false;   // a SETUP/OUT token arrived
    bool response_pending_    = false;
    std::vector<uint8_t> response_;

    void respond(const std::vector<uint8_t>& bytes) {
        response_         = bytes;
        response_pending_ = true;
    }

    void handle_token(uint8_t pid, const std::vector<uint8_t>& bytes) {
        // Token shape: PID, field low byte, {CRC5, field high 3 bits}.
        // A bare SOF PID is the low-speed keep-alive.
        if (pid == PID_SOF && bytes.size() == 1) {
            keepalives++;
            return;
        }
        if (bytes.size() != 3) {
            crc_failures++;
            return;
        }
        uint16_t field = (uint16_t)(bytes[1] | ((bytes[2] & 0x7) << 8));
        if (token_crc5(field) != (bytes[2] >> 3)) {
            crc_failures++;
            return;
        }

        if (pid == PID_SOF) {
            sof_frames.push_back(field);
            return;
        }

        tokens.push_back({ pid, (uint8_t)(field & 0x7f),
                           (uint8_t)(field >> 7) });
        data_stage_expected_ = (pid != PID_IN);
        if (pid == PID_IN) {
            switch (in_response) {
            case RSP_NAK:   respond({ pid_byte(PID_NAK) });   break;
            case RSP_STALL: respond({ pid_byte(PID_STALL) }); break;
            case RSP_DATA:
                respond(data_packet(in_toggle ? PID_DATA1 : PID_DATA0,
                                    in_payload));
                break;
            default: break;
            }
        }
    }

    void handle_data(uint8_t pid, const std::vector<uint8_t>& bytes) {
        (void)pid;
        // Host data arrives only as a SETUP/OUT data stage.
        if (!data_stage_expected_)
            return;
        data_stage_expected_ = false;

        // Wire shape: PID, payload, CRC16 low then high.
        if (bytes.size() < 3) {
            crc_failures++;
            return;
        }
        std::vector<uint8_t> payload(bytes.begin() + 1, bytes.end() - 2);
        uint16_t crc = (uint16_t)(bytes[bytes.size() - 2] |
                                  (bytes[bytes.size() - 1] << 8));
        if (data_crc16(payload) != crc) {
            crc_failures++;
            return;
        }
        out_payload = payload;

        switch (out_response) {
        case RSP_ACK:   respond({ pid_byte(PID_ACK) });   break;
        case RSP_NAK:   respond({ pid_byte(PID_NAK) });   break;
        case RSP_STALL: respond({ pid_byte(PID_STALL) }); break;
        case RSP_DATA:  respond(data_packet(PID_DATA0, { 0x5a })); break;
        default: break;
        }
    }
};

#endif // USB_DEVICE_SIM_H
