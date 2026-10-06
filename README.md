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

## Measured (2026-10-06, one board, Linux)

Bench: this adapter (PLCA coordinator, id 0 of 2) ═T1S═ a 100BASE-TX converter ─ an ESP32-S3 W5500
node that sinks and blasts 1472 B UDP and reports what it saw.

| check | result |
|---|---|
| enumeration | `cdc_ncm` interface `enx…`, no driver; link follows the T1S link |
| ping across T1S to the W5500 node | 10/10, avg 4.0 ms (min 3.2) |
| live capture (`tshark -i enx…`) | bus traffic as it happens |
| **computer → bus**, 1472 B | 4 / 6 / 8 Mbit/s offered: all delivered (8: 3392 of 3397); ceiling **8.4 Mbit/s** |
| **bus → computer**, 1472 B | 4 / 6 Mbit/s: all delivered; 7 offered: 6.2; **above ≈6 Mbit/s the adapter drops** (counted as `t1s -> usb dropped`) and delivery falls to ≈4 |

Bus → computer is the weaker direction: every frame must go out through the ESP32-S3's USB
**full-speed** device, whose TinyUSB driver fills the FIFO from the CPU and, as built into the
Arduino core, has one NCM IN buffer. Not tested: macOS, Windows.

## Build and flash

Arduino core esp32 3.3.0. The USB must be in **TinyUSB** mode (not "Hardware CDC and JTAG"):

```bash
FQBN="esp32:esp32:esp32s3:PSRAM=opi,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
arduino-cli compile --fqbn "$FQBN" t1s_usb_nic
arduino-cli upload  --fqbn "$FQBN" -p /dev/ttyACM0 t1s_usb_nic
```

The first flash from a board in "Hardware CDC and JTAG" firmware works over that port as usual.
Afterwards the board is a TinyUSB device (`303a:1001 ESP32S3_DEV`) and a plain upload cannot reset
it. Open its console at **1200 baud** and close it: it reboots into the ROM downloader (a
`USB JTAG/serial debug unit` port). Flash with esptool, then reset with `--before usb-reset`
(a plain hard reset leaves it in the downloader):

```bash
python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',1200); time.sleep(0.2); s.close()"
esptool --port /dev/ttyACM0 --before no-reset write-flash -z 0x10000 t1s_usb_nic.ino.bin
esptool --port /dev/ttyACM0 --before usb-reset --after hard-reset chip-id
```

Holding **BOOT** while plugging in also enters the downloader.

## Use

```bash
ip -br link | grep enx                          # the new interface
sudo nmcli dev set enx… managed no              # else NetworkManager keeps trying DHCP and drops the address
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

- Computer → bus up to ≈8.4 Mbit/s, bus → computer ≈6 Mbit/s (USB full speed, see above); the
  T1S line rate is 10 Mbit/s.
- PLCA is set from the console, not `ethtool`.
- No hardware timestamps.

## Files

| | |
|---|---|
| `t1s_usb_nic/t1s_usb_nic.ino` | the firmware |
| `t1s_usb_nic/pins.h` | HAT signals on the T-ETH-Elite header |
| `t1s_usb_nic/src/lan865x/` | Espressif's LAN865x driver (Apache-2.0), vendored, see `VENDORED.md` |

MIT (the driver: Apache-2.0).
