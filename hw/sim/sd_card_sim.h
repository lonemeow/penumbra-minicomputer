// Simulated SD card (SPI mode) for Penumbra testbench
//
// Byte-level SD-SPI protocol emulator backed by a disk image file.
// Models the full-duplex SPI exchange: each MOSI byte in produces
// one MISO byte out. Connects to sim_spi.sv signals via the
// testbench (o_cmd_valid → exchange(), result → i_resp_data).
//
// Supported commands (minimal set for boot):
//   CMD0  (GO_IDLE_STATE)     → R1 with idle bit
//   CMD6  (SEND_SWITCH_FUNC)  → R1 + data token + 64 bytes + 2 CRC
//   CMD8  (SEND_IF_COND)      → R7 (R1 + 4 bytes echo)
//   CMD9  (SEND_CSD)          → R1 + data token + 16 CSD bytes + 2 CRC
//   CMD10 (SEND_CID)          → R1 + data token + 16 CID bytes + 2 CRC
//   CMD55 (APP_CMD)           → R1 (prefix for ACMD)
//   ACMD41 (SD_SEND_OP_COND)  → R1 (clears idle after init)
//   ACMD51 (SD_SEND_SCR)      → R1 + data token + 8 SCR bytes + 2 CRC
//   CMD58 (READ_OCR)          → R1 + 4-byte OCR
//   CMD17 (READ_SINGLE_BLOCK) → R1 + data token + 512 bytes + 2 CRC
//   CMD24 (WRITE_SINGLE_BLOCK)→ R1, then accepts data token + 512 + CRC
//
// Usage:
//   SdCardSim sd("disk.img");
//   sd.select(cs0_is_low);           // call each cycle
//   if (cmd_valid) {
//       uint8_t miso = sd.exchange(mosi_byte);
//       // present miso on i_resp_data for SPI controller to latch
//   }

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

class SdCardSim {
public:
    explicit SdCardSim(const char* image_path) {
        if (image_path && image_path[0]) {
            img_ = fopen(image_path, "r+b");
            if (!img_) {
                img_ = fopen(image_path, "rb");
                if (img_)
                    fprintf(stderr, "[SD] opened '%s' read-only\n", image_path);
                else
                    fprintf(stderr, "[SD] warning: cannot open '%s'\n", image_path);
            } else {
                fprintf(stderr, "[SD] opened '%s' read-write\n", image_path);
            }
            if (img_) {
                fseek(img_, 0, SEEK_END);
                long sz = ftell(img_);
                total_sectors_ = (sz > 0) ? (uint32_t)(sz / 512) : 0;
                fseek(img_, 0, SEEK_SET);
                fprintf(stderr, "[SD] image size: %lu bytes (%u sectors)\n",
                        (unsigned long)sz, total_sectors_);
            }
        }
    }

    ~SdCardSim() {
        if (img_) fclose(img_);
    }

    void set_trace(bool on) { trace_ = on; }

    // Called each cycle with current CS0 state (active low: true = selected)
    void select(bool selected) {
        if (trace_ && selected != selected_)
            fprintf(stderr, "[SD] CS0 %s\n", selected ? "assert" : "deassert");
        if (!selected && selected_) {
            // CS deasserted — reset command state
            cmd_pos_ = 0;
            state_ = S_IDLE;
            resp_queue_.clear();
            resp_idx_ = 0;
            write_pos_ = 0;
        }
        selected_ = selected;
    }

    // Full-duplex SPI byte exchange: MOSI byte in, MISO byte out.
    // Called once per o_cmd_valid pulse. The returned byte is held
    // on i_resp_data for the SPI controller to latch.
    uint8_t exchange(uint8_t mosi) {
        if (!selected_ || !img_) return 0xFF;  // no card = MISO high

        // Abort-mid-stream escape: if a write is pending its data
        // token and a command-start byte arrives instead, the host
        // driver is retrying after a failure — drop back to IDLE so
        // the new command is recognised.
        if (state_ == S_RECEIVING_DATA && write_pos_ == 0 &&
            !multi_write_active_ &&
            (mosi & 0xC0) == 0x40) {
            state_ = S_IDLE;
        }

        // Multi-block stream abort.  Any CMD-frame-start byte
        // ((mosi & 0xC0) == 0x40) observed during a CMD18 read stream
        // or at a CMD25 start-of-block boundary terminates the stream
        // and routes the byte through normal command receive.  This
        // mirrors how real SD cards behave when the host issues CMD12
        // mid-stream.
        if ((multi_read_active_ || multi_write_active_) &&
            cmd_pos_ == 0 && (mosi & 0xC0) == 0x40 &&
            (state_ == S_SENDING_DATA || state_ == S_SENDING_RESPONSE ||
             (state_ == S_RECEIVING_DATA && write_pos_ == 0))) {
            multi_read_active_ = false;
            multi_write_active_ = false;
            resp_queue_.clear(); resp_idx_ = 0;
            write_pos_ = 0;
            state_ = S_IDLE;
        }

        uint8_t miso = 0xFF;
        switch (state_) {
        case S_IDLE:
        case S_RECEIVING_CMD:
            // Accumulate command bytes; MISO is 0xFF during command
            if (cmd_pos_ == 0) {
                if ((mosi & 0xC0) != 0x40) {
                    if (trace_) fprintf(stderr, "[SD] idle: mosi=0x%02X (skip)\n", mosi);
                    return 0xFF;  // not a command start byte
                }
                state_ = S_RECEIVING_CMD;
            }
            cmd_buf_[cmd_pos_++] = mosi;
            if (cmd_pos_ == 6) {
                uint8_t cmd = cmd_buf_[0] & 0x3F;
                if (trace_) fprintf(stderr, "[SD] CMD%d arg=0x%08X\n", cmd, cmd_arg());
                process_command();
                cmd_pos_ = 0;
            }
            return 0xFF;

        case S_SENDING_RESPONSE:
        case S_SENDING_DATA:
            // Firmware clocks dummy 0xFF to read response bytes
            if (resp_idx_ < resp_queue_.size()) {
                miso = resp_queue_[resp_idx_++];
                if (trace_ && resp_idx_ <= 8)
                    fprintf(stderr, "[SD] resp[%zu]: 0x%02X\n", resp_idx_-1, miso);
                if (resp_idx_ >= resp_queue_.size()) {
                    resp_queue_.clear();
                    resp_idx_ = 0;
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
                return miso;
            }
            state_ = S_IDLE;
            return 0xFF;

        case S_RECEIVING_DATA:
            if (multi_write_active_) {
                // CMD25 multi-block write framing differs from CMD24:
                //   - 0xFC token: start of a block (vs 0xFE for CMD24)
                //   - 0xFD token: stop transmission
                //   - 0xFF pad bytes between blocks are skipped
                // After each block we queue [0x05 data response, 0x00
                // brief busy, 0xFF release] so the kernel's strict
                // busy-wait variant (requires seeing busy before
                // exit) is satisfied.
                if (write_pos_ == 0) {
                    if (mosi == 0xFD) {
                        // Stop tran: brief busy then idle.
                        multi_write_active_ = false;
                        resp_queue_.push_back(0x00);
                        resp_queue_.push_back(0xFF);
                        state_ = S_SENDING_DATA;
                        return 0xFF;
                    }
                    if (mosi == 0xFC) {
                        write_buf_[write_pos_++] = mosi;
                        return 0xFF;
                    }
                    return 0xFF;   // pad byte, skip
                }
                write_buf_[write_pos_++] = mosi;
                if (write_pos_ == 515) {
                    if (img_ && multi_lba_ < total_sectors_) {
                        fseek(img_, (long)multi_lba_ * 512L, SEEK_SET);
                        fwrite(&write_buf_[1], 1, 512, img_);
                        fflush(img_);
                    }
                    multi_lba_++;
                    write_pos_ = 0;
                    resp_queue_.push_back(0x05);   // accepted
                    resp_queue_.push_back(0x00);   // brief busy
                    resp_queue_.push_back(0xFF);   // release
                    state_ = S_SENDING_RESPONSE;
                    post_resp_state_ = S_RECEIVING_DATA;
                    return 0xFF;
                }
                return 0xFF;
            }
            // CMD24 single-block write: host first sends at least one
            // 0xFF pad byte (Nwr gap), then the 0xFE data-start token,
            // then 512 data bytes + 2 CRC.  Skip pads until the token
            // arrives so the buffer indexing stays aligned.
            if (write_pos_ == 0 && mosi != 0xFE) return 0xFF;
            write_buf_[write_pos_++] = mosi;
            if (write_pos_ == 515) {
                flush_write();
                // Queue the data-response token to be returned on
                // the next exchange, not inline with the final CRC
                // byte.  Real cards leave >= 1 byte (Ncrc) of 0xFF
                // between CRC and response; our driver polls.
                resp_queue_.push_back(0x05);
                state_ = S_SENDING_RESPONSE;
                return 0xFF;
            }
            return 0xFF;
        }

        return 0xFF;
    }

    bool is_present() const { return img_ != nullptr; }

private:
    // SD-SPI command indices
    static constexpr uint8_t CMD0   = 0;
    static constexpr uint8_t CMD6   = 6;
    static constexpr uint8_t CMD8   = 8;
    static constexpr uint8_t CMD9   = 9;
    static constexpr uint8_t CMD10  = 10;
    static constexpr uint8_t CMD12  = 12;   // STOP_TRANSMISSION
    static constexpr uint8_t CMD17  = 17;
    static constexpr uint8_t CMD18  = 18;   // READ_BLOCK_MULTIPLE
    static constexpr uint8_t CMD23  = 23;   // ACMD23 SET_WR_BLK_ERASE_COUNT
    static constexpr uint8_t CMD24  = 24;
    static constexpr uint8_t CMD25  = 25;   // WRITE_BLOCK_MULTIPLE
    static constexpr uint8_t CMD55  = 55;
    static constexpr uint8_t CMD58  = 58;
    static constexpr uint8_t ACMD41 = 41;
    static constexpr uint8_t ACMD51 = 51;

    // R1 response bits
    static constexpr uint8_t R1_IDLE          = 0x01;
    static constexpr uint8_t R1_ILLEGAL_CMD   = 0x04;
    static constexpr uint8_t R1_ADDRESS_ERROR = 0x20;

    enum State {
        S_IDLE,
        S_RECEIVING_CMD,
        S_SENDING_RESPONSE,
        S_SENDING_DATA,
        S_RECEIVING_DATA,
    };

    // SD-SPI command dispatch.  Called when cmd_buf_[] holds a
    // complete 6-byte command.  Queues response into resp_queue_.
    void process_command() {
        resp_idx_ = 0;
        resp_queue_.clear();

        bool prev_app_cmd = app_cmd_;
        app_cmd_ = false;
        state_ = S_SENDING_RESPONSE;
        post_resp_state_ = S_IDLE;

        uint8_t cmd = cmd_buf_[0] & 0x3F;

        switch (cmd) {
        case CMD0:
            initialized_ = false;
            resp_queue_.push_back(R1_IDLE);
            break;

        case CMD8:
            // R7: R1 + echo back 4-byte argument (voltage + check pattern)
            resp_queue_.push_back(R1_IDLE);
            resp_queue_.push_back(cmd_buf_[1]);
            resp_queue_.push_back(cmd_buf_[2]);
            resp_queue_.push_back(cmd_buf_[3]);
            resp_queue_.push_back(cmd_buf_[4]);
            break;

        case CMD6: {
            // SD_SEND_SWITCH_FUNC — 64-byte mode-status response.
            // We advertise Group 1 (access mode) as SDR12-only, which
            // lets select_transfer_mode pick best_func = 0 and skip
            // the mode-1 follow-up that would actually switch speeds.
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
                break;
            }
            resp_queue_.push_back(0x00);         // R1 OK
            resp_queue_.push_back(0xFF);         // Nwr gap
            resp_queue_.push_back(0xFE);         // data token
            uint8_t sfs[64] = {};
            sfs[13] = 0x01;                      // Group 1: SDR12 only
            for (int i = 0; i < 64; i++)
                resp_queue_.push_back(sfs[i]);
            resp_queue_.push_back(0x00);         // dummy CRC16
            resp_queue_.push_back(0x00);
            state_ = S_SENDING_DATA;
            break;
        }

        case CMD55:
            app_cmd_ = true;
            resp_queue_.push_back(initialized_ ? 0x00 : R1_IDLE);
            break;

        case ACMD41:
            if (!prev_app_cmd) {
                resp_queue_.push_back(R1_ILLEGAL_CMD);
            } else if (!initialized_) {
                // First ACMD41: card leaves idle → ready
                initialized_ = true;
                resp_queue_.push_back(0x00);
            } else {
                resp_queue_.push_back(0x00);
            }
            break;

        case ACMD51: {
            // SD Configuration Register — 8 bytes.  MI sdmmc queries
            // this during enumeration; SCR_STRUCTURE must be 0 or 1
            // for sdmmc_mem_decode_scr to accept it.
            if (!prev_app_cmd) {
                resp_queue_.push_back(R1_ILLEGAL_CMD);
                break;
            }
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
                break;
            }
            resp_queue_.push_back(0x00);         // R1 OK
            resp_queue_.push_back(0xFF);         // Nwr gap
            resp_queue_.push_back(0xFE);         // data token
            uint8_t scr[8] = {
                0x02,   // [0] SCR_STRUCTURE=0, SD_SPEC=2 (v2.00)
                0x01,   // [1] 1-bit bus width only, no security
                0x00, 0x00,
                0, 0, 0, 0,
            };
            for (int i = 0; i < 8; i++)
                resp_queue_.push_back(scr[i]);
            resp_queue_.push_back(0x00);         // dummy CRC16
            resp_queue_.push_back(0x00);
            state_ = S_SENDING_DATA;
            break;
        }

        case CMD9: {
            // CSD register (version 2.0 for SDHC)
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
                break;
            }
            resp_queue_.push_back(0x00);         // R1 OK
            resp_queue_.push_back(0xFF);         // Nwr gap
            resp_queue_.push_back(0xFE);         // data token
            // 16-byte CSD v2.0 structure
            uint8_t csd[16] = {};
            csd[0] = 0x40;                      // CSD_STRUCTURE = 1 (v2.0)
            csd[1] = 0x0E;                      // TAAC
            csd[2] = 0x00;                      // NSAC
            csd[3] = 0x32;                      // TRAN_SPEED = 25 MHz
            csd[4] = 0x5B;                      // CCC high
            csd[5] = 0x59;                      // CCC low + READ_BL_LEN=9
            // C_SIZE: capacity = (C_SIZE + 1) * 512 KB
            // total_sectors_ / 1024 - 1 = C_SIZE
            uint32_t c_size = (total_sectors_ > 1024)
                            ? (total_sectors_ / 1024 - 1) : 0;
            csd[6]  = 0x00;
            csd[7]  = (c_size >> 16) & 0x3F;
            csd[8]  = (c_size >> 8) & 0xFF;
            csd[9]  = c_size & 0xFF;
            csd[10] = 0x7F;                     // various flags
            csd[11] = 0x80;
            csd[12] = 0x0A;
            csd[13] = 0x40;
            csd[14] = 0x00;
            csd[15] = 0x01;                     // CRC (don't care in SPI)
            for (int i = 0; i < 16; i++)
                resp_queue_.push_back(csd[i]);
            resp_queue_.push_back(0x00);         // dummy CRC16
            resp_queue_.push_back(0x00);
            state_ = S_SENDING_DATA;
            break;
        }

        case CMD10: {
            // CID register — 16 bytes of manufacturer-assigned
            // identification.  The MI sdmmc stack reads CID before
            // CSD and refuses to enumerate the card if this command
            // returns illegal-command.  Values are arbitrary but
            // structurally valid so sdmmc_decode_cid extracts
            // non-empty strings for the boot banner.
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
                break;
            }
            resp_queue_.push_back(0x00);         // R1 OK
            resp_queue_.push_back(0xFF);         // Nwr gap
            resp_queue_.push_back(0xFE);         // data token
            uint8_t cid[16] = {
                0x03,                             // [ 0] MID: SanDisk-ish
                'P', 'S',                         // [ 1.. 2] OID: "PS"
                'R','T','L','_','_',              // [ 3.. 7] PNM: "RTL__"
                0x10,                             // [ 8] PRV: v1.0
                0x00, 0x00, 0x00, 0x01,           // [ 9..12] PSN: serial 1
                0x01, 0x64,                       // [13..14] MDT: 2026/04
                0x01,                             // [15] CRC7 + end bit
            };
            for (int i = 0; i < 16; i++)
                resp_queue_.push_back(cid[i]);
            resp_queue_.push_back(0x00);         // dummy CRC16
            resp_queue_.push_back(0x00);
            state_ = S_SENDING_DATA;
            break;
        }

        case CMD58:
            // R3: R1 + 4-byte OCR (SDHC, power-up complete, 3.3V)
            resp_queue_.push_back(initialized_ ? 0x00 : R1_IDLE);
            resp_queue_.push_back(0x40);  // CCS=1 (SDHC), power-up complete
            resp_queue_.push_back(0xFF);
            resp_queue_.push_back(0x80);
            resp_queue_.push_back(0x00);
            break;

        case CMD17: {
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
                break;
            }
            if (cmd_arg() >= total_sectors_) {
                resp_queue_.push_back(R1_ADDRESS_ERROR);
                break;
            }
            resp_queue_.push_back(0x00);         // R1 OK
            resp_queue_.push_back(0xFF);         // Nwr gap
            resp_queue_.push_back(0xFF);
            resp_queue_.push_back(0xFE);         // data token
            // 512 bytes from disk image (SDHC: arg is LBA)
            uint8_t sector[512];
            memset(sector, 0xFF, sizeof(sector));
            fseek(img_, (long)cmd_arg() * 512L, SEEK_SET);
            size_t n = fread(sector, 1, 512, img_);
            (void)n;
            for (size_t i = 0; i < 512; i++)
                resp_queue_.push_back(sector[i]);
            resp_queue_.push_back(0x00);         // dummy CRC16
            resp_queue_.push_back(0x00);
            state_ = S_SENDING_DATA;
            break;
        }

        case CMD24:
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
            } else if (cmd_arg() >= total_sectors_) {
                resp_queue_.push_back(R1_ADDRESS_ERROR);
            } else {
                // Host expects to read R1 before the data phase.
                // Stay in S_SENDING_RESPONSE until R1 is drained,
                // then transition to S_RECEIVING_DATA via
                // post_resp_state_.
                resp_queue_.push_back(0x00);     // R1 OK
                write_lba_ = cmd_arg();
                write_pos_ = 0;
                post_resp_state_ = S_RECEIVING_DATA;
            }
            break;

        case CMD12:
            // STOP_TRANSMISSION.  The actual stream termination
            // happens in the abort-on-CMD-frame check in exchange();
            // by the time we get here, multi_read_active_ /
            // multi_write_active_ are already false.  Just ack.
            resp_queue_.push_back(0x00);
            break;

        case CMD18: {
            // READ_BLOCK_MULTIPLE.  Stream blocks until the host
            // sends CMD12.  Pre-load R1 + gap bytes + first block;
            // refill_multi_read() pushes subsequent blocks as
            // resp_queue_ drains.
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
                break;
            }
            if (cmd_arg() >= total_sectors_) {
                resp_queue_.push_back(R1_ADDRESS_ERROR);
                break;
            }
            resp_queue_.push_back(0x00);         // R1 OK
            resp_queue_.push_back(0xFF);         // Nwr gap
            multi_read_active_ = true;
            multi_lba_ = cmd_arg();
            push_read_block(multi_lba_++);
            state_ = S_SENDING_DATA;
            break;
        }

        case CMD23:
            // ACMD23 SET_WR_BLK_ERASE_COUNT (or CMD23
            // SET_BLOCK_COUNT — same opcode, distinguished by the
            // preceding CMD55).  Both are advisory hints; we don't
            // track the count.  Acknowledge so the MI sdmmc layer's
            // pre-erase prefix doesn't fail before CMD25.
            resp_queue_.push_back(initialized_ ? 0x00 : R1_IDLE);
            break;

        case CMD25:
            // WRITE_BLOCK_MULTIPLE.  Like CMD24 but S_RECEIVING_DATA
            // (when multi_write_active_ is set) accepts 0xFC start
            // tokens for each block and 0xFD for stop tran.
            if (!initialized_) {
                resp_queue_.push_back(R1_IDLE | R1_ILLEGAL_CMD);
            } else if (cmd_arg() >= total_sectors_) {
                resp_queue_.push_back(R1_ADDRESS_ERROR);
            } else {
                resp_queue_.push_back(0x00);     // R1 OK
                multi_write_active_ = true;
                multi_lba_ = cmd_arg();
                write_pos_ = 0;
                post_resp_state_ = S_RECEIVING_DATA;
            }
            break;

        default:
            resp_queue_.push_back(R1_ILLEGAL_CMD);
            break;
        }
    }

    // Push 0xFE + 512 data bytes + 2 CRC for the given LBA into
    // resp_queue_.  Shared by CMD18 initial-block setup and by
    // refill_multi_read() for subsequent blocks.
    void push_read_block(uint32_t lba) {
        resp_queue_.push_back(0xFE);             // data start token
        uint8_t sector[512];
        memset(sector, 0xFF, sizeof(sector));
        if (img_ && lba < total_sectors_) {
            fseek(img_, (long)lba * 512L, SEEK_SET);
            size_t n = fread(sector, 1, 512, img_);
            (void)n;
        }
        for (size_t i = 0; i < 512; i++)
            resp_queue_.push_back(sector[i]);
        resp_queue_.push_back(0x00);             // dummy CRC16
        resp_queue_.push_back(0x00);
    }

    // Refill resp_queue_ with the next block of a CMD18 stream, or
    // end the stream if we've run off the end of the device.
    void refill_multi_read() {
        resp_queue_.clear();
        resp_idx_ = 0;
        if (multi_lba_ >= total_sectors_) {
            multi_read_active_ = false;
            state_ = S_IDLE;
            return;
        }
        push_read_block(multi_lba_++);
        state_ = S_SENDING_DATA;
    }

    void flush_write() {
        // write_buf_[0] = data token (0xFE)
        // write_buf_[1..512] = sector data
        // write_buf_[513..514] = CRC (ignored)
        if (img_ && write_lba_ < total_sectors_) {
            fseek(img_, (long)write_lba_ * 512L, SEEK_SET);
            fwrite(&write_buf_[1], 1, 512, img_);
            fflush(img_);
        }
        state_ = S_IDLE;
        write_pos_ = 0;
    }

    uint32_t cmd_arg() const {
        return ((uint32_t)cmd_buf_[1] << 24) |
               ((uint32_t)cmd_buf_[2] << 16) |
               ((uint32_t)cmd_buf_[3] << 8)  |
               (uint32_t)cmd_buf_[4];
    }

    FILE*    img_ = nullptr;
    uint32_t total_sectors_ = 0;
    bool     trace_ = false;
    bool     selected_ = false;
    bool     initialized_ = false;
    bool     app_cmd_ = false;
    State    state_ = S_IDLE;
    State    post_resp_state_ = S_IDLE;

    uint8_t  cmd_buf_[6] = {};
    int      cmd_pos_ = 0;

    std::vector<uint8_t> resp_queue_;
    size_t   resp_idx_ = 0;

    // CMD24 write state
    uint8_t  write_buf_[515] = {};
    int      write_pos_ = 0;
    uint32_t write_lba_ = 0;

    // Multi-block (CMD18 read, CMD25 write) streaming state.  Both
    // flags are cleared either when the host sends 0xFD (CMD25 only)
    // or when a CMD frame arrives mid-stream (abort).
    bool     multi_read_active_ = false;
    bool     multi_write_active_ = false;
    uint32_t multi_lba_ = 0;
};
