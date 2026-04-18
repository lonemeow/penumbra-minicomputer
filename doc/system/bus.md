# Penumbra Bus — System Programmer's Reference

## Address Map

The physical address space uses a fixed layout decoded from the top address bits.

```
0x0000_0000 ┌─────────────────────┐
            │ System RAM          │  Cached, hardwired at base 0
            ├─────────────────────┤
            │ Expansion RAM       │  Autoconfigured
            ├─────────────────────┤
            │ (unmapped)          │  Bus fault if accessed
0xFDFF_FFFF └─────────────────────┘
0xFE00_0000 ┌─────────────────────┐
            │ Config Space (32 B) │  Autoconfig registers
0xFE00_001F └─────────────────────┘
            │ (unmapped)          │
0xFEFF_FFFF └─────────────────────┘
0xFF00_0000 ┌─────────────────────┐
            │ I/O Region (16 MB)  │  Always uncached (C=0)
            │ Hardwired: UART     │
            │ Autoconfigured: rest│
0xFFFE_FFFF └─────────────────────┘
0xFFFF_0000 ┌─────────────────────┐
            │ Boot ROM (64 KB)    │  Always uncached (C=0), hardwired
0xFFFF_FFFF └─────────────────────┘
```

## Device Discovery (Autoconfig)

Devices on the Penumbra Bus support automatic discovery and address assignment.

### Autoconfig Loop

1. Assert bus reset via `BUSCTL.RST`.
2. Delay for at least 100 µs.
3. Enable config mode via `BUSCTL.CFG_EN`.
4. Install a bus-fault-ignore trap handler.
5. Read `CFG_CLASS` at `0xFE00_0000`. If it bus faults, no more devices.
6. Read `CFG_SIZE`, `CFG_ID`, and `CFG_NAME`.
7. Assign a base address and write it to `0xFE00_001C`.
8. **Toggle `CFG_EN`** (write 0 then 1) to advance the daisy chain.
9. Repeat from step 5.

### Config Space Registers (0xFE00_0000)

| Address | R/W | Name | Description |
|---------|-----|------|-------------|
| `0xFE00_0000` | R | **CFG_CLASS** | 1=MEMORY, 2=UART, 3=SPI, 4=SD |
| `0xFE00_0004` | R | **CFG_SIZE** | Required space in bytes (power-of-2) |
| `0xFE00_0008` | R | **CFG_ID** | Manufacturer + Product ID |
| `0xFE00_000C` | R | **CFG_NAME0-3**| 16-byte device name |
| `0xFE00_001C` | W | **CFG_BASE** | Write assigned base address to enable |

## Interrupt Model

Penumbra uses a **PCI-style shared IRQ** line. All bus devices wire-OR their interrupt outputs onto a single CPU input (**VEC_EXT_IRQ**, vector 9). The kernel must poll each device's status register to identify the source.
