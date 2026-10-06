# t1s-usb-nic

A LilyGo **T-ETH-Elite** (ESP32-S3) with a **LAN8651** 10BASE-T1S HAT, plugged into a computer,
becomes a **USB network adapter on a 10BASE-T1S bus**. No media converter, no driver to install.

```
computer ──USB (CDC-NCM)── ESP32-S3 ──SPI 20 MHz── LAN8651 ══ 10BASE-T1S bus (PLCA or CSMA/CD)
            └─ + CDC serial console (PLCA, counters)
```

- The ESP32 runs **no IP**: Ethernet frames pass USB ↔ LAN8651 unchanged. The computer owns the
  addresses (DHCP, static, whatever it likes).
- The LAN8651 is **promiscuous**, so the new interface sees every frame on the bus:
  **Wireshark on it is a live T1S capture**.
- The same USB cable carries a **serial console** for PLCA and status.

## Measured (2026-10-06, one board)

| check | result |
|---|---|
| Linux enumeration | `cdc_ncm` interface `enx…` appears, link follows the T1S link |
| PLCA | coordinator (id 0 of 2), beacons seen |
| ping across T1S + a 100BASE-TX converter to an ESP32 W5500 node | 10/10, avg 4.0 ms (min 3.2) |
| live capture on the interface (`tshark -i enx…`) | bus traffic seen as it happens |

Not measured yet: throughput (expected ≈8 Mbit/s each way: USB full speed and the in-spec SPI
clock both sit there), macOS/Windows.

## Build and flash

Arduino core esp32 3.3.0. The USB must be in **TinyUSB** mode (not "Hardware CDC and JTAG"):

```bash
FQBN="esp32:esp32:esp32s3:PSRAM=opi,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
arduino-cli compile --fqbn "$FQBN" t1s_usb_nic
arduino-cli upload  --fqbn "$FQBN" -p /dev/ttyACM0 t1s_usb_nic
```

The first flash from a board in "Hardware CDC and JTAG" firmware works over that port as usual.
Afterwards the board shows up as a TinyUSB device (`303a:1001 ESP32S3_DEV`); if an upload cannot
reset it, hold **BOOT** while plugging in.

## Use

```bash
ip -br link | grep enx                          # the new interface
sudo ip addr add 192.168.100.10/24 dev enx…     # any address on the bus's subnet
ping 192.168.100.66
sudo wireshark -i enx…                          # live T1S bus
```

Console (`/dev/ttyACM*`, any baud):

| command | |
|---|---|
| `status` | T1S link, SPI clock, PLCA as read back (and whether beacons are seen), frame counters both ways |
| `plca <id> [count]` | PLCA node id (0 = coordinator) and node count, applied at once |
| `csma` | PLCA off |
| `save` / `reboot` | keep the PLCA setting / restart |

PLCA settings live in NVS under the same keys as the `t1s_node` firmware of
[elite-t1s-hat](https://github.com/hwkim3330/elite-t1s-hat), so a board moved between the two keeps them.

LED (IO38): solid = beacons seen (or link up with PLCA off), fast blink = no beacons, slow blink = no link.

## How

- **USB**: TinyUSB's CDC-NCM class is compiled into the Arduino core; the sketch registers its
  descriptor (`USB_INTERFACE_CUSTOM`) from a global constructor, before Arduino starts USB.
  The host-side MAC is derived from the efuse (locally administered).
- **Computer → bus**: `tud_network_recv_cb` copies the frame into a queue; a task transmits it on
  the LAN8651, so the USB task never waits on SPI.
- **Bus → computer**: the LAN8651 driver's input path hands each frame to a queue; a task passes it
  to `tud_network_xmit` when an IN buffer is free (dropped after 50 ms if the host stopped reading).
- **SPI clock**: never above the LAN8651's 25 MHz (DS60001734F, Table 9-9). The ESP32-S3 rounds
  25 MHz to 26.67 (80/3), so the request is walked down until what runs is in spec: 20 MHz.

## Limits

- USB full speed (12 Mbit/s) and SPI 20 MHz: roughly 8 Mbit/s each way, below the 10 Mbit/s line rate.
- PLCA is set from the console, not `ethtool`.
- No hardware timestamps.

## Files

| | |
|---|---|
| `t1s_usb_nic/t1s_usb_nic.ino` | the firmware |
| `t1s_usb_nic/pins.h` | HAT signals on the T-ETH-Elite header |
| `t1s_usb_nic/src/lan865x/` | Espressif's LAN865x driver (Apache-2.0), vendored, see `VENDORED.md` |

MIT (the driver: Apache-2.0).
