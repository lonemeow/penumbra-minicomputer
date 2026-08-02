// USB mass-storage device function (simulation)
//
// A UsbFunctionSim personality: a SCSI direct-access disk behind
// Bulk-Only Transport, backed by a host file image (the +sdcard=
// pattern).  The transport walks CBW → [data] → CSW per command; the
// SCSI layer serves the command subset a host block-device stack
// needs — identify, capacity, block read/write, sense reporting.
//
// Like the wire layer, deliberately free of any Verilated or terminal
// dependency.

#ifndef USB_MSC_SIM_H
#define USB_MSC_SIM_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "usb_device_sim.h"

class UsbMassStorageSim : public UsbFunctionSim {
public:
    // USB interface identity (config descriptor fields).
    static const uint8_t USB_CLASS_MSC       = 0x08;
    static const uint8_t MSC_SUBCLASS_SCSI   = 0x06;  // transparent SCSI
    static const uint8_t MSC_PROTO_BULK_ONLY = 0x50;
    static const uint8_t EP_ATTR_BULK        = 0x02;

    // MSC class requests (interface recipient).
    static const uint8_t REQ_GET_MAX_LUN = 0xFE;
    static const uint8_t REQ_BO_RESET    = 0xFF;

    // Bulk-Only Transport wrappers.
    static const uint32_t CBW_SIGNATURE = 0x43425355;  // 'USBC'
    static const uint32_t CSW_SIGNATURE = 0x53425355;  // 'USBS'
    static const size_t   CBW_LEN = 31;
    static const size_t   CSW_LEN = 13;
    static const uint8_t  CBW_FLAG_DATA_IN = 0x80;
    static const uint8_t  CSW_PASS = 0;
    static const uint8_t  CSW_FAIL = 1;

    // SCSI operation codes (the served subset).
    static const uint8_t SCSI_TEST_UNIT_READY  = 0x00;
    static const uint8_t SCSI_REQUEST_SENSE    = 0x03;
    static const uint8_t SCSI_INQUIRY          = 0x12;
    static const uint8_t SCSI_MODE_SENSE_6     = 0x1A;
    static const uint8_t SCSI_START_STOP_UNIT  = 0x1B;
    static const uint8_t SCSI_PREVENT_ALLOW    = 0x1E;
    static const uint8_t SCSI_READ_CAPACITY_10 = 0x25;
    static const uint8_t SCSI_READ_10          = 0x28;
    static const uint8_t SCSI_WRITE_10         = 0x2A;
    static const uint8_t SCSI_SYNC_CACHE_10    = 0x35;

    // SCSI sense: key and additional sense code (ASCQ always 0 in the
    // served subset).
    static const uint8_t SENSE_MEDIUM_ERROR    = 0x03;
    static const uint8_t SENSE_ILLEGAL_REQUEST = 0x05;
    static const uint8_t ASC_UNRECOVERED_READ  = 0x11;
    static const uint8_t ASC_INVALID_OPCODE    = 0x20;
    static const uint8_t ASC_LBA_OUT_OF_RANGE  = 0x21;
    static const size_t  SENSE_FIXED_LEN = 18;

    static const uint32_t DISK_BLOCK = 512;

    bool attach(const char* image_path) {
        img_ = fopen(image_path, "r+b");
        if (!img_) {
            fprintf(stderr, "[USBDISK] cannot open '%s'\n", image_path);
            return false;
        }
        fseek(img_, 0, SEEK_END);
        long sz = ftell(img_);
        capacity_ = (sz > 0) ? (uint32_t)(sz / DISK_BLOCK) : 0;
        fprintf(stderr, "[USBDISK] '%s': %u blocks of %u bytes\n",
                image_path, capacity_, DISK_BLOCK);
        return true;
    }

    ~UsbMassStorageSim() override {
        if (img_)
            fclose(img_);
    }

    // ── UsbFunctionSim ──────────────────────────────────────────────

    const std::vector<uint8_t>& config_descriptor() const override {
        // config(9) + interface(9) + 2 × endpoint(7); wTotalLength 32.
        static const std::vector<uint8_t> d{
            9, UsbDeviceSim::DESC_CONFIGURATION,
            32, 0,      // wTotalLength
            1,          // bNumInterfaces
            1,          // bConfigurationValue
            0,          // iConfiguration
            0x80,       // bmAttributes: bus-powered
            0x32,       // bMaxPower: 100 mA

            9, UsbDeviceSim::DESC_INTERFACE,
            0, 0,       // bInterfaceNumber, bAlternateSetting
            2,          // bNumEndpoints
            USB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTO_BULK_ONLY,
            0,          // iInterface

            7, UsbDeviceSim::DESC_ENDPOINT,
            0x80 | UsbDeviceSim::EP_BULK_IN,   // bEndpointAddress: IN
            EP_ATTR_BULK,
            UsbDeviceSim::BULK_MAX_PKT, 0,     // wMaxPacketSize
            0,          // bInterval

            7, UsbDeviceSim::DESC_ENDPOINT,
            UsbDeviceSim::EP_BULK_OUT,         // bEndpointAddress: OUT
            EP_ATTR_BULK,
            UsbDeviceSim::BULK_MAX_PKT, 0,     // wMaxPacketSize
            0,          // bInterval
        };
        return d;
    }

    bool control_request(const uint8_t* req,
                         std::vector<uint8_t>& data_in) override {
        uint8_t type = req[0] & UsbDeviceSim::RT_TYPE_MASK;
        uint8_t recipient = req[0] & UsbDeviceSim::RT_RECIP_MASK;
        bool dev_to_host =
            (req[0] & UsbDeviceSim::RT_DIR_DEV_TO_HOST) != 0;

        if (type != UsbDeviceSim::RT_TYPE_CLASS ||
            recipient != UsbDeviceSim::RT_RECIP_INTERFACE)
            return false;

        if (dev_to_host && req[1] == REQ_GET_MAX_LUN) {
            data_in = { 0 };        // a single LUN: the disk itself
            return true;
        }
        if (!dev_to_host && req[1] == REQ_BO_RESET) {
            // Abandon any open transport state.  Endpoint toggles and
            // halts belong to the wire layer; the host clears those
            // separately.
            stage_ = BOT_CBW;
            return true;
        }
        return false;
    }

    void bulk_out(const std::vector<uint8_t>& payload) override {
        switch (stage_) {
        case BOT_CBW:
            parse_cbw(payload);
            break;
        case BOT_DATA_OUT:
            data_.insert(data_.end(), payload.begin(), payload.end());
            if (data_.size() >= (size_t)rw_blocks_ * DISK_BLOCK) {
                disk_write(rw_lba_, data_);
                moved_ = (uint32_t)data_.size();
                stage_ = BOT_CSW;
            }
            break;
        default:
            break;      // data with no command open; drop it
        }
    }

    InResult bulk_in(InTransfer& xfer) override {
        switch (stage_) {
        case BOT_DATA_IN:
            xfer.data = data_;
            xfer.short_end = short_end_;
            stage_ = BOT_DATA_IN_BUSY;
            return IN_DATA;
        case BOT_HALT_IN:
            // No data for a host that expects some: halt the pipe;
            // the CSW follows the host's clear-halt (Bulk-Only
            // recovery).
            stage_ = BOT_CSW;
            return IN_HALT;
        case BOT_CSW:
            xfer.data = csw_packet();
            xfer.short_end = false;    // 13 bytes end short naturally
            stage_ = BOT_CSW_BUSY;
            return IN_DATA;
        default:
            return IN_NONE;
        }
    }

    void bulk_in_done() override {
        if (stage_ == BOT_DATA_IN_BUSY)
            stage_ = BOT_CSW;
        else if (stage_ == BOT_CSW_BUSY)
            stage_ = BOT_CBW;
    }

    void configured() override { stage_ = BOT_CBW; }

private:
    // The transport walks CBW → [data] → CSW per command; the _BUSY
    // states cover a transfer handed to the wire layer but not yet
    // fully taken by the host.
    enum BotStage {
        BOT_CBW,            // expecting a command wrapper
        BOT_DATA_OUT,       // collecting WRITE payload
        BOT_DATA_IN,        // command data armed for the host
        BOT_DATA_IN_BUSY,   //   ...and handed to the wire layer
        BOT_HALT_IN,        // command produced no data the host wants
        BOT_CSW,            // status wrapper armed
        BOT_CSW_BUSY,       //   ...and handed to the wire layer
    };
    BotStage stage_ = BOT_CBW;

    FILE* img_ = nullptr;
    uint32_t capacity_ = 0;         // disk size in DISK_BLOCK units

    std::vector<uint8_t> data_;     // data-in payload / data-out sink
    bool short_end_ = false;        // data-in reads short of the ask
    uint32_t moved_ = 0;            // bytes moved, for the CSW residue
    uint8_t tag_[4] = { 0, 0, 0, 0 };  // dCBWTag, echoed into the CSW
    uint32_t dlen_ = 0;             // dCBWDataTransferLength
    uint8_t status_ = CSW_PASS;
    uint32_t rw_lba_ = 0;           // WRITE(10) start block
    uint32_t rw_blocks_ = 0;        //   and block count (0 = no data)

    // Latched SCSI sense, returned and cleared by REQUEST SENSE.
    uint8_t sense_key_ = 0, sense_asc_ = 0;

    // Fail the current command: CHECK CONDITION in the CSW now, this
    // sense for the REQUEST SENSE that follows.
    void scsi_fail(uint8_t key, uint8_t asc) {
        status_ = CSW_FAIL;
        sense_key_ = key;
        sense_asc_ = asc;
    }

    // Image block I/O.  A read that runs off the image end reports a
    // medium error — the bounds check should have rejected it.
    bool disk_read(uint32_t lba, uint32_t blocks) {
        data_.assign((size_t)blocks * DISK_BLOCK, 0);
        fseek(img_, (long)lba * DISK_BLOCK, SEEK_SET);
        return fread(data_.data(), 1, data_.size(), img_) ==
               data_.size();
    }

    void disk_write(uint32_t lba, const std::vector<uint8_t>& data) {
        fseek(img_, (long)lba * DISK_BLOCK, SEEK_SET);
        fwrite(data.data(), 1, data.size(), img_);
        fflush(img_);
    }

    // Parse a Command Block Wrapper and execute its SCSI command,
    // then set the transport stage the result implies: a data-out
    // command waits for the WRITE payload, produced data arms the
    // data-in stage, a data-in command that produced nothing halts
    // the bulk-in endpoint, and everything else goes straight to
    // status.
    void parse_cbw(const std::vector<uint8_t>& cbw) {
        if (cbw.size() != CBW_LEN ||
            UsbDeviceSim::get_le32(&cbw[0]) != CBW_SIGNATURE) {
            fprintf(stderr, "[USBDISK] malformed CBW (%zu bytes)\n",
                    cbw.size());
            return;
        }
        std::copy(cbw.begin() + 4, cbw.begin() + 8, tag_);
        dlen_ = UsbDeviceSim::get_le32(&cbw[8]);
        bool dev_to_host = (cbw[12] & CBW_FLAG_DATA_IN) != 0;

        data_.clear();
        moved_ = 0;
        rw_blocks_ = 0;
        status_ = CSW_PASS;
        scsi_execute(&cbw[15]);

        if (status_ == CSW_PASS && !dev_to_host && rw_blocks_ != 0) {
            stage_ = BOT_DATA_OUT;
        } else if (dev_to_host && dlen_ > 0 && !data_.empty()) {
            if (data_.size() > dlen_)
                data_.resize(dlen_);       // never exceed the ask
            moved_ = (uint32_t)data_.size();
            short_end_ = data_.size() < dlen_;
            stage_ = BOT_DATA_IN;
        } else if (dev_to_host && dlen_ > 0) {
            stage_ = BOT_HALT_IN;
        } else {
            stage_ = BOT_CSW;
        }
    }

    std::vector<uint8_t> csw_packet() const {
        std::vector<uint8_t> v(CSW_LEN, 0);
        UsbDeviceSim::put_le32(&v[0], CSW_SIGNATURE);
        std::copy(tag_, tag_ + 4, v.begin() + 4);
        UsbDeviceSim::put_le32(&v[8], dlen_ - moved_);   // residue
        v[12] = status_;
        return v;
    }

    // Execute one SCSI command block.  A data-in command loads data_
    // with its response; WRITE arms rw_lba_/rw_blocks_ for the
    // payload stage; a failure latches sense via scsi_fail and
    // leaves no data.
    void scsi_execute(const uint8_t* cb) {
        switch (cb[0]) {
        case SCSI_TEST_UNIT_READY:
        case SCSI_START_STOP_UNIT:
        case SCSI_PREVENT_ALLOW:
        case SCSI_SYNC_CACHE_10:
            break;      // pass, no data

        case SCSI_REQUEST_SENSE: {
            std::vector<uint8_t> s(SENSE_FIXED_LEN, 0);
            s[0] = 0x70;                  // current error, fixed format
            s[2] = sense_key_;
            s[7] = SENSE_FIXED_LEN - 8;   // additional length
            s[12] = sense_asc_;
            data_ = s;
            sense_key_ = sense_asc_ = 0;
            break;
        }

        case SCSI_INQUIRY: {
            std::vector<uint8_t> q(36, 0);
            // q[0]: direct-access device, q[1]: not removable.
            q[2] = 0x02;                  // SCSI-2
            q[3] = 0x02;                  // response data format 2
            q[4] = (uint8_t)(q.size() - 5);  // additional length
            static const char id[] =
                "PENUMBRA"                //  8: vendor
                "USB SIM DISK    "        // 16: product
                "1.0 ";                   //  4: revision
            std::copy(id, id + 28, q.begin() + 8);
            data_ = q;
            break;
        }

        case SCSI_MODE_SENSE_6:
            // Header only: 3 more bytes, default medium, writable, no
            // block descriptors.
            data_ = { 3, 0, 0, 0 };
            break;

        case SCSI_READ_CAPACITY_10: {
            data_.assign(8, 0);
            UsbDeviceSim::put_be32(&data_[0],
                                   capacity_ ? capacity_ - 1 : 0);
            UsbDeviceSim::put_be32(&data_[4], DISK_BLOCK);
            break;
        }

        case SCSI_READ_10:
        case SCSI_WRITE_10: {
            uint32_t start_lba = UsbDeviceSim::get_be32(&cb[2]);
            uint32_t count = UsbDeviceSim::get_be16(&cb[7]);
            uint32_t end_lba = start_lba + count;

            if (end_lba < start_lba || end_lba > capacity_) {
                scsi_fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE);
            } else if (cb[0] == SCSI_READ_10) {
                if (!disk_read(start_lba, count))
                    scsi_fail(SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ);
            } else {
                rw_lba_ = start_lba;
                rw_blocks_ = count;
            }
            break;
        }

        default:
            scsi_fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE);
            break;
        }
    }
};

#endif // USB_MSC_SIM_H
