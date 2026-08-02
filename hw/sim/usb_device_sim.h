// Byte-level USB device responder (simulation)
//
// Models an attached USB device at the packet-byte level: it consumes
// complete host packets as they appear on the wire (PID byte, token field
// or payload, CRC) and produces the device's complete response packet, or
// silence. The transaction pacing — turnaround delays, byte rates, the
// receive window — belongs to the harness that owns the seam; this class
// is pure protocol.
//
// This is the shared USB device model: the MAC testbench drives it
// against the RTL seam, and the machine_sim PHY model and the ISS's
// register-level USBHC reuse it.  UsbDeviceSim is the wire layer only;
// device-class behavior plugs in behind the UsbFunctionSim seam below
// (usb_msc_sim.h is the mass-storage disk; the HID boot keyboard is
// the next function).  Any interactive input a function ever takes
// must not come from terminal stdin — the UART console owns stdin; a
// keyboard front-end needs its own channel.
//
// Deliberately free of any Verilated or terminal dependency.

#ifndef USB_DEVICE_SIM_H
#define USB_DEVICE_SIM_H

#include <algorithm>
#include <cstdint>
#include <vector>

// The seam between the USB wire layer (UsbDeviceSim) and a device
// function — the class-specific personality behind the endpoints.
// The wire layer owns packets, CRCs, the control-endpoint machine,
// endpoint addressing, data toggles, transfer chunking, and endpoint
// halt state; a function owns its configuration descriptor, its
// class/vendor control requests, and transfer-level bulk behavior.
// Parallel functions (a mass-storage disk, a HID keyboard) plug into
// the same layer.
class UsbFunctionSim {
public:
    virtual ~UsbFunctionSim() {}

    // The configuration blob this function's device presents.
    virtual const std::vector<uint8_t>& config_descriptor() const = 0;

    // A class or vendor SETUP; req is the 8-byte setup packet.  Fill
    // data_in for a device-to-host data stage (the layer bounds it by
    // wLength); return false to STALL the request.
    virtual bool control_request(const uint8_t* req,
                                 std::vector<uint8_t>& data_in) = 0;

    // One toggle-verified payload packet from the bulk-out endpoint.
    virtual void bulk_out(const std::vector<uint8_t>& payload) = 0;

    // The layer pulls the next bulk-in transfer when the endpoint is
    // idle: IN_NONE has the host retry later (NAK), IN_HALT stalls
    // the endpoint until the host clears the halt, IN_DATA opens the
    // transfer walk.  short_end marks a transfer that must read short
    // to the host; the layer supplies the zero-length terminator when
    // such a transfer ends on a packet boundary.
    enum InResult { IN_NONE, IN_DATA, IN_HALT };
    struct InTransfer {
        std::vector<uint8_t> data;
        bool short_end = false;
    };
    virtual InResult bulk_in(InTransfer& xfer) = 0;

    // The host took the last chunk of the open bulk-in transfer.
    virtual void bulk_in_done() = 0;

    // A configuration event: endpoints reset, transport state clears.
    virtual void configured() {}
};

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

    // bmRequestType fields (USB 2.0 chapter 9). The three fields form a
    // namespace: bRequest codes are only meaningful within their
    // (type, recipient) scope, and the direction bit names which
    // transfer shape the request can coherently have.
    static const uint8_t RT_DIR_DEV_TO_HOST = 0x80;  // bit 7 set
    static const uint8_t RT_TYPE_MASK       = 0x60;
    static const uint8_t RT_TYPE_STANDARD   = 0x00;
    static const uint8_t RT_TYPE_CLASS      = 0x20;
    static const uint8_t RT_RECIP_MASK      = 0x1F;
    static const uint8_t RT_RECIP_DEVICE    = 0x00;
    static const uint8_t RT_RECIP_INTERFACE = 0x01;
    static const uint8_t RT_RECIP_ENDPOINT  = 0x02;

    // Standard bRequest codes and descriptor types (the subset a boot
    // enumeration touches).
    static const uint8_t REQ_CLEAR_FEATURE     = 1;
    static const uint8_t REQ_SET_ADDRESS       = 5;
    static const uint8_t REQ_GET_DESCRIPTOR    = 6;
    static const uint8_t REQ_SET_CONFIGURATION = 9;
    static const uint8_t FEAT_ENDPOINT_HALT    = 0;
    static const uint8_t DESC_DEVICE           = 1;
    static const uint8_t DESC_CONFIGURATION    = 2;
    static const uint8_t DESC_STRING           = 3;
    static const uint8_t DESC_INTERFACE        = 4;
    static const uint8_t DESC_ENDPOINT         = 5;

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

    // ── Enumeration personality ──────────────────────────────────────
    //
    // With enumerate=true the scripted knobs above are replaced by a
    // control-endpoint state machine: SETUP requests are parsed and
    // dispatched, IN tokens walk the data stage in bMaxPacketSize0
    // chunks with alternating toggles, and the status stage closes the
    // transfer. This is what host software enumerates against — the
    // machine_sim integration test and the ISS share it.
    bool enumerate = false;

    // The device descriptor a GET_DESCRIPTOR(device) returns: a
    // full-speed, 8-byte-EP0 device with one configuration.
    static const std::vector<uint8_t>& device_descriptor() {
        static const std::vector<uint8_t> d{
            0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x08,
            0x34, 0x12, 0x01, 0x00, 0x00, 0x01, 0x01, 0x02,
            0x00, 0x01,
        };
        return d;
    }

    // The configuration blob (config + interface descriptors in one
    // read, per the USB layout): one bus-powered configuration
    // (value 1) holding a single vendor-class interface with no
    // endpoints beyond control — the minimal shape a host stack will
    // enumerate and configure. A device-class personality (the
    // CDC-ECM NIC) replaces this blob wholesale.
    static const std::vector<uint8_t>& config_descriptor() {
        static const std::vector<uint8_t> d{
            0x09, 0x02, 0x12, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
            0x09, 0x04, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
        };
        return d;
    }


    // String descriptors: index 0 is the LANGID table (en-US); 1 and 2
    // are the manufacturer/product indices the device descriptor
    // carries. The product string is deliberately 16 bytes — an exact
    // multiple of bMaxPacketSize0 — so an over-long read exercises the
    // ZLP transfer termination. Null for an index the device lacks.
    static const std::vector<uint8_t>* string_descriptor(uint8_t index) {
        static const std::vector<uint8_t> lang{ 0x04, 0x03, 0x09, 0x04 };
        static const std::vector<uint8_t> mfg{
            0x12, 0x03, 'P', 0, 'e', 0, 'n', 0, 'u', 0,
            'm', 0, 'b', 0, 'r', 0, 'a', 0,
        };
        static const std::vector<uint8_t> prod{
            0x10, 0x03, 'S', 0, 'i', 0, 'm', 0, ' ', 0,
            'D', 0, 'e', 0, 'v', 0,
        };
        switch (index) {
        case 0: return &lang;
        case 1: return &mfg;
        case 2: return &prod;
        default: return nullptr;
        }
    }

    // SET_CONFIGURATION latch (0 = unconfigured), harness-checkable.
    uint8_t configuration = 0;

    // bMaxPacketSize0 — must match byte 7 of device_descriptor(); the
    // control data stage is chunked and terminated against this size.
    static const size_t EP0_MAX_PKT = 8;

    // ── Device function attachment ───────────────────────────────────
    //
    // A UsbFunctionSim behind the seam gives the device its
    // class-specific behavior: the configuration descriptor, class
    // control requests, and the bulk endpoints.  Without one the
    // device is the bare enumerable vendor device.  The bulk endpoint
    // numbers are a layer convention every function's descriptor must
    // repeat.
    static const uint8_t EP_BULK_IN   = 1;     // device-to-host, 0x81
    static const uint8_t EP_BULK_OUT  = 2;     // host-to-device, 0x02
    static const size_t  BULK_MAX_PKT = 64;

    void set_function(UsbFunctionSim* f) { function_ = f; }

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

    // Byte-order accessors for parsing and building wire structures —
    // USB control fields are little-endian, SCSI command fields are
    // big-endian, and hand-rolled shifts at every use site invite
    // off-by-one field offsets.
    static uint16_t get_le16(const uint8_t* p) {
        return (uint16_t)(p[0] | (p[1] << 8));
    }
    static uint32_t get_le32(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    static uint16_t get_be16(const uint8_t* p) {
        return (uint16_t)((p[0] << 8) | p[1]);
    }
    static uint32_t get_be32(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    static void put_le32(uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }
    static void put_be32(uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
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
            if (enumerate) {
                // The ACK belongs to whichever endpoint's data packet
                // the host just received.
                if (last_in_ep_ == 0)
                    ctrl_handle_ack();
                else if (function_ && last_in_ep_ == EP_BULK_IN)
                    bulk_handle_ack();
            }
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
    bool setup_stage_ = false;           // ...and the token was SETUP
    bool response_pending_    = false;
    std::vector<uint8_t> response_;

    // Control-endpoint state (enumeration personality).
    enum CtrlStage {
        CTRL_IDLE,        // no transfer open
        CTRL_DATA_IN,     // device-to-host data stage in progress
        CTRL_STATUS_OUT,  // awaiting the host's zero-length OUT status
        CTRL_STATUS_IN,   // host expects a zero-length IN status
        CTRL_STALLED,     // request refused; STALL until the next SETUP
    };
    CtrlStage ctrl_stage_ = CTRL_IDLE;
    std::vector<uint8_t> ctrl_data_;   // data-stage bytes still to send
    size_t ctrl_pos_ = 0;
    bool ctrl_toggle_ = true;          // data stage starts at DATA1
    bool ctrl_zlp_ = false;            // transfer owes a terminating ZLP
    uint8_t addr_cur_ = 0;             // device address on the bus
    uint8_t addr_pending_ = 0;         // latched by SET_ADDRESS, applied
    bool addr_apply_ = false;          //   after its status stage

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

        if (enumerate) {
            // A real device answers only its own address; everything
            // else on the bus is not for it.
            if ((field & 0x7f) != addr_cur_)
                return;
            token_endp_ = (uint8_t)(field >> 7);
            data_stage_expected_ = (pid != PID_IN);
            setup_stage_ = (pid == PID_SETUP);
            if (pid == PID_IN) {
                last_in_ep_ = token_endp_;
                if (token_endp_ == 0)
                    ctrl_handle_in();
                else if (function_ && token_endp_ == EP_BULK_IN)
                    bulk_handle_in();
                else
                    respond({ pid_byte(PID_STALL) });
            }
            return;
        }

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

        if (enumerate) {
            if (function_ && token_endp_ == EP_BULK_OUT) {
                bulk_handle_out(pid, payload);
                return;
            }
            if (setup_stage_) {
                // A SETUP supersedes whatever transfer was open; the
                // dispatch below decides the new transfer's shape.
                setup_stage_  = false;
                ctrl_pos_     = 0;
                ctrl_toggle_  = true;   // the data stage starts at DATA1
                ctrl_data_.clear();
                ctrl_zlp_     = false;
                addr_apply_   = false;
                ctrl_dispatch_setup(payload);
                respond({ pid_byte(PID_ACK) });   // setup stage always ACKs
            } else if (ctrl_stage_ == CTRL_STATUS_OUT) {
                // The zero-length OUT status closes a read transfer.
                ctrl_stage_ = CTRL_IDLE;
                respond({ pid_byte(PID_ACK) });
            } else if (ctrl_stage_ == CTRL_STALLED) {
                respond({ pid_byte(PID_STALL) });
            } else {
                respond({ pid_byte(PID_ACK) });
            }
            return;
        }

        switch (out_response) {
        case RSP_ACK:   respond({ pid_byte(PID_ACK) });   break;
        case RSP_NAK:   respond({ pid_byte(PID_NAK) });   break;
        case RSP_STALL: respond({ pid_byte(PID_STALL) }); break;
        case RSP_DATA:  respond(data_packet(PID_DATA0, { 0x5a })); break;
        default: break;
        }
    }

    // ── Control-endpoint plumbing (enumeration personality) ──────────

    // An IN token at the control endpoint: offer the current data-stage
    // chunk (bMaxPacketSize0 bytes), the status ZLP, or a STALL. The
    // walk advances only on the host's ACK — a re-sent IN re-offers the
    // same chunk, which is USB's retransmission.
    void ctrl_handle_in() {
        switch (ctrl_stage_) {
        case CTRL_DATA_IN: {
            // The chunk at the walk position; an exhausted walk still
            // in this stage owes the terminating ZLP, which the empty
            // chunk produces naturally.
            size_t n = ctrl_data_.size() - ctrl_pos_;
            if (n > EP0_MAX_PKT)
                n = EP0_MAX_PKT;
            std::vector<uint8_t> chunk(ctrl_data_.begin() + (long)ctrl_pos_,
                                       ctrl_data_.begin() +
                                           (long)(ctrl_pos_ + n));
            respond(data_packet(ctrl_toggle_ ? PID_DATA1 : PID_DATA0,
                                chunk));
            break;
        }
        case CTRL_STATUS_IN:
            respond(data_packet(PID_DATA1, {}));
            break;
        default:
            respond({ pid_byte(PID_STALL) });
            break;
        }
    }

    // The host acknowledged our last data packet: commit the walk.
    void ctrl_handle_ack() {
        if (ctrl_stage_ == CTRL_DATA_IN) {
            size_t n = ctrl_data_.size() - ctrl_pos_;
            if (n > EP0_MAX_PKT)
                n = EP0_MAX_PKT;
            ctrl_pos_ += n;
            ctrl_toggle_ = !ctrl_toggle_;
            if (ctrl_pos_ >= ctrl_data_.size()) {
                // A full final chunk with a ZLP owed keeps the stage
                // open for the empty terminating packet; anything else
                // (a short chunk, or the acked ZLP itself) ends the
                // data stage.
                if (ctrl_zlp_ && n == EP0_MAX_PKT)
                    ctrl_zlp_ = false;
                else
                    ctrl_stage_ = CTRL_STATUS_OUT;
            }
        } else if (ctrl_stage_ == CTRL_STATUS_IN) {
            if (addr_apply_) {
                addr_cur_ = addr_pending_;
                addr_apply_ = false;
            }
            ctrl_stage_ = CTRL_IDLE;
        }
    }

    // Open a device-to-host data stage: offer `data` bounded by the
    // request's wLength, walked in bMaxPacketSize0 chunks by
    // ctrl_handle_in. The transfer's end is implicit: it runs to
    // wLength exactly, or ends early on the first short packet —
    // and a bounded length that is an exact multiple of the packet
    // size has no short packet of its own, so the early end must be
    // an explicit zero-length packet.
    void ctrl_start_data_in(const std::vector<uint8_t>& data,
                            uint16_t wLength) {
        size_t count = std::min<size_t>(wLength, data.size());
        ctrl_data_.assign(data.begin(), data.begin() + (long)count);
        ctrl_stage_ = CTRL_DATA_IN;
        ctrl_zlp_ = count != wLength && count % EP0_MAX_PKT == 0;
    }

    // Dispatch one parsed SETUP request — the 8 setup-stage bytes:
    //   req[0] bmRequestType (bit 7: 1 = device-to-host data stage)
    //   req[1] bRequest
    //   req[2] wValue low    req[3] wValue high
    //   req[4] wIndex low    req[5] wIndex high
    //   req[6] wLength low   req[7] wLength high
    // Decides the transfer's shape by setting the control state:
    //   * device-to-host data stage — ctrl_start_data_in with the
    //     descriptor and the request's wLength
    //   * no data stage (a pure action) — latch the action and set
    //     ctrl_stage_ = CTRL_STATUS_IN (SET_ADDRESS must latch into
    //     addr_pending_/addr_apply_, never addr_cur_ directly: the
    //     device answers the status stage at its OLD address, and the
    //     plumbing applies the change when that stage completes)
    //   * anything this device does not implement — CTRL_STALLED
    void ctrl_dispatch_setup(const std::vector<uint8_t>& req) {
        bool dev_to_host = (req[0] & RT_DIR_DEV_TO_HOST) != 0;
        uint8_t type = req[0] & RT_TYPE_MASK;
        uint8_t recipient = req[0] & RT_RECIP_MASK;
        uint8_t request = req[1];
        uint16_t wValue = get_le16(&req[2]);
        uint16_t wLength = get_le16(&req[6]);

        if (type == RT_TYPE_STANDARD && recipient == RT_RECIP_DEVICE) {
            if (dev_to_host) {
                switch (request) {
                case REQ_GET_DESCRIPTOR: {
                    uint8_t desc_type = uint8_t(wValue >> 8);
                    uint8_t desc_index = uint8_t(wValue & 0xff);
                    if (desc_type == DESC_DEVICE && desc_index == 0) {
                        ctrl_start_data_in(device_descriptor(), wLength);
                        return;
                    }
                    if (desc_type == DESC_CONFIGURATION && desc_index == 0) {
                        ctrl_start_data_in(function_
                                               ? function_->config_descriptor()
                                               : config_descriptor(),
                                           wLength);
                        return;
                    }
                    if (desc_type == DESC_STRING) {
                        const std::vector<uint8_t>* s =
                            string_descriptor(desc_index);
                        if (s != nullptr) {
                            ctrl_start_data_in(*s, wLength);
                            return;
                        }
                    }
                    break;
                }
                }
            } else {
                switch (request) {
                case REQ_SET_ADDRESS: {
                    addr_pending_ = req[2];
                    addr_apply_ = true;
                    ctrl_stage_ = CTRL_STATUS_IN;
                    return;
                }
                case REQ_SET_CONFIGURATION: {
                    // Only configuration 1 exists; selecting it (or
                    // deconfiguring with 0) is a pure action.  Every
                    // endpoint restarts at DATA0 and any open bulk
                    // state is abandoned, per the configuration-event
                    // semantics.
                    if ((wValue & 0xff) <= 1) {
                        configuration = uint8_t(wValue & 0xff);
                        bulk_in_toggle_ = bulk_out_toggle_ = false;
                        bulk_in_halted_ = false;
                        in_open_ = false;
                        if (function_)
                            function_->configured();
                        ctrl_stage_ = CTRL_STATUS_IN;
                        return;
                    }
                    break;
                }
                }
            }
        }

        if (type == RT_TYPE_STANDARD && recipient == RT_RECIP_ENDPOINT &&
            !dev_to_host && request == REQ_CLEAR_FEATURE &&
            wValue == FEAT_ENDPOINT_HALT) {
            // Clearing a halt restarts the endpoint at DATA0 and
            // releases the halt latch; a function whose transfer was
            // pending behind the halt gets pulled again on the next
            // IN token.
            uint8_t ep = req[4] & 0xf;
            if (ep == EP_BULK_IN) {
                bulk_in_toggle_ = false;
                bulk_in_halted_ = false;
                in_open_ = false;
            } else if (ep == EP_BULK_OUT) {
                bulk_out_toggle_ = false;
            }
            ctrl_stage_ = CTRL_STATUS_IN;
            return;
        }

        if (type != RT_TYPE_STANDARD && function_ != nullptr) {
            // Class and vendor requests belong to the device function.
            std::vector<uint8_t> data;
            if (function_->control_request(req.data(), data)) {
                if (dev_to_host)
                    ctrl_start_data_in(data, wLength);
                else
                    ctrl_stage_ = CTRL_STATUS_IN;
                return;
            }
        }

        ctrl_stage_ = CTRL_STALLED;
    }

    // ── Bulk endpoints (the device-function seam) ───────────────────

    UsbFunctionSim* function_ = nullptr;
    uint8_t token_endp_ = 0;        // endpoint of the last token
    uint8_t last_in_ep_ = 0;        // endpoint whose data awaits ACK
    bool bulk_in_toggle_ = false;   // next DATAx we send on bulk IN
    bool bulk_out_toggle_ = false;  // next DATAx we expect on bulk OUT
    bool bulk_in_halted_ = false;   // STALL every IN until clear-halt

    // The open bulk-in walk: one function transfer, chunked at
    // BULK_MAX_PKT with alternating toggles, advanced on the host's
    // ACK exactly like the control data stage.
    bool in_open_ = false;
    std::vector<uint8_t> in_data_;
    size_t in_pos_ = 0;
    bool in_zlp_ = false;           // owes a zero-length terminator

    // An IN token at the bulk-in endpoint: the next chunk of the open
    // walk, a freshly pulled transfer, a halt handshake, or NAK when
    // the function has nothing to send.
    void bulk_handle_in() {
        if (bulk_in_halted_) {
            respond({ pid_byte(PID_STALL) });
            return;
        }
        if (!in_open_) {
            UsbFunctionSim::InTransfer xfer;
            switch (function_->bulk_in(xfer)) {
            case UsbFunctionSim::IN_NONE:
                respond({ pid_byte(PID_NAK) });
                return;
            case UsbFunctionSim::IN_HALT:
                bulk_in_halted_ = true;
                respond({ pid_byte(PID_STALL) });
                return;
            case UsbFunctionSim::IN_DATA:
                in_open_ = true;
                in_data_ = std::move(xfer.data);
                in_pos_ = 0;
                // A transfer that must read short but ends on a
                // packet boundary owes an explicit zero-length end.
                in_zlp_ = xfer.short_end &&
                          in_data_.size() % BULK_MAX_PKT == 0;
                break;
            }
        }
        size_t n = in_data_.size() - in_pos_;
        if (n > BULK_MAX_PKT)
            n = BULK_MAX_PKT;
        std::vector<uint8_t> chunk(in_data_.begin() + (long)in_pos_,
                                   in_data_.begin() + (long)(in_pos_ + n));
        respond(data_packet(bulk_in_toggle_ ? PID_DATA1 : PID_DATA0,
                            chunk));
    }

    // The host acknowledged our last bulk-in packet: commit the walk.
    void bulk_handle_ack() {
        if (!in_open_)
            return;
        bulk_in_toggle_ = !bulk_in_toggle_;
        size_t n = in_data_.size() - in_pos_;
        if (n > BULK_MAX_PKT)
            n = BULK_MAX_PKT;
        in_pos_ += n;
        if (in_pos_ >= in_data_.size()) {
            // A full final chunk with a zero-length end owed keeps the
            // walk open for the empty packet; anything else (a short
            // chunk, or the acked terminator itself) closes it.
            if (in_zlp_ && n == BULK_MAX_PKT) {
                in_zlp_ = false;
            } else {
                in_open_ = false;
                function_->bulk_in_done();
            }
        }
    }

    // Host data at the bulk-out endpoint.  A toggle-mismatched packet
    // is the host retransmitting after a lost ACK: acknowledge again
    // and discard.
    void bulk_handle_out(uint8_t pid, const std::vector<uint8_t>& payload) {
        bool toggle = (pid == PID_DATA1);
        if (toggle == bulk_out_toggle_) {
            bulk_out_toggle_ = !bulk_out_toggle_;
            function_->bulk_out(payload);
        }
        respond({ pid_byte(PID_ACK) });
    }
};

#endif // USB_DEVICE_SIM_H
