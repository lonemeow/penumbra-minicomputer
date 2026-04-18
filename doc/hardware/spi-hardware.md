# SPI Controller — Hardware Design

## Block Diagram

```
                  Bus Interface
                 ┌──────────────────────────────┐
  mem_addr ─────►│ Register decode (addr[4:2])   │
  mem_wdata ────►│  CAP / STATUS / CONTROL /     │
  mem_we ───────►│  DATA / XFER_COUNT /          │
  mem_re ───────►│  IRQ_STATUS / IRQ_ENABLE      │
  mem_rdata ◄───│                                │
  mem_busy ◄────│                                │
                 └─────┬──────────┬──────────────┘
                       │          │
              ┌────────▼──┐  ┌───▼──────────┐
              │  TX FIFO  │  │   RX FIFO    │
              │ (param    │  │  (param      │
              │  depth)   │  │   depth)     │
              └────┬──────┘  └───▲──────────┘
                   │             │
              ┌────▼─────────────┴──────────┐
              │     Transfer Engine          │
              │  ┌────────────────────────┐  │
              │  │  SPI Shift Register    │  │
              │  │  8-bit, MSB first      │  │
              │  └──────────┬─────────────┘  │
              │  FAST/SLOW clock divider     │
              │  Byte counter (XFER_COUNT)   │
              │  CPOL/CPHA logic             │
              └────────────┬────────────────┘
                           │
               ┌───────────┼───────────┐
               │           │           │
            o_sclk     o_mosi      i_miso
               │
            o_cs0, o_cs1

                     ┌──────────────┐
  o_irq ◄──────────│  IRQ Logic   │
                     │  XFER_DONE   │◄── engine complete (latched, W1C)
                     │  RX_THRESH   │◄── rx_level >= depth/2 (live)
                     │  TX_THRESH   │◄── tx_level <= depth/2 (live)
                     │  & IRQ_ENABLE│
                     └──────────────┘
```

## Transfer Engine State Machine

The engine has four states. It stalls in LOAD_BYTE if the TX FIFO is empty, and in STORE_BYTE if the RX FIFO is full — the SPI clock pauses during stalls.

```
           ┌─────────┐
    ───────► S_IDLE  │ XFER_COUNT write with START=1
           │         ├──────────────────────┐
           └────▲────┘                      ▼
                │                    ┌──────────────┐
                │                    │ S_LOAD_BYTE  │ Pop TX FIFO
                │                    │ (stall if    │ Load shift register
                │                    │  TX empty)   │
                │                    └──────┬───────┘
                │                           ▼
                │                    ┌──────────────┐
                │                    │ S_SHIFTING   │ Clock 8 bits
                │                    └──────┬───────┘
                │                           ▼
                │                    ┌──────────────┐
                │  count=0           │ S_STORE_BYTE │ Push RX byte
                ├────────────────────│ (stall if    │ Decrement count
                │                    │  RX full)    │
                │                    └──────┬───────┘
                │                           │ count>0
        set XFER_DONE              (back to S_LOAD_BYTE)
```

## 74xx Feasibility

The SPI core (shift register + clock divider + control) is ~4 chips:
- 74HC595 or 74HC165: 8-bit shift register.
- 74HC4040: clock divider counter.
- 74HC74: control flip-flops.
- 74HC00/74HC32: gating logic.

The FIFO adds ~2 chips per direction for a small discrete FIFO (e.g., 74HC40105 16-deep FIFO). The transfer engine byte counter is one more counter chip. Total for a 16-byte FIFO build: ~12 chips plus register decoding.
