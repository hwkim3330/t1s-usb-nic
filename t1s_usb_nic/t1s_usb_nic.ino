// t1s_usb_nic -- a LilyGo T-ETH-Elite (ESP32-S3) with a LAN8651 10BASE-T1S HAT, plugged into a
// computer, becomes a USB network adapter on the T1S bus.
//
//   computer ──USB (CDC-NCM, no driver needed on Linux/macOS)── ESP32-S3 ──SPI── LAN8651 ══ T1S bus
//
// The ESP32 does no IP at all: Ethernet frames go USB -> LAN8651 and LAN8651 -> USB unchanged,
// the LAN8651 in promiscuous mode, so the computer owns the addresses and sees the whole bus
// (Wireshark on the new interface is a live T1S capture). The same USB also carries a CDC serial
// console for PLCA and status.
//
// Build: USB Mode "USB-OTG (TinyUSB)", USB CDC On Boot enabled -- see README.md.
#include <Arduino.h>
#include <Preferences.h>
#include <esp_eth.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include "USB.h"
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include "pins.h"
#include "src/lan865x/esp_eth_mac_lan865x.h"
#include "src/lan865x/esp_eth_phy_lan865x.h"

// The LAN8651's SCLK maximum (DS60001734F, Table 9-9). The ESP32-S3 divides 80 MHz by an integer
// and rounds to the nearest divider, so the request is walked down until what runs is in spec:
// 20 MHz (80/4) is the fastest. That carries ~8 Mbit/s each way, about what USB full speed
// (12 Mbit/s raw) leaves after its own overhead, so neither link starves the other.
constexpr int kSpiMaxHz = 25 * 1000 * 1000;
constexpr uint8_t kPlcaOff = 255;

static Preferences gPrefs;
static uint8_t gPlcaId = kPlcaOff, gPlcaCount = 8;   // shared NVS keys with t1s_node ("t1s"/id, cnt)
static esp_eth_handle_t gEth = nullptr;
static esp_eth_mac_t *gMac = nullptr;
static volatile bool gLinkUp = false;
static float gSpiMhz = 0;

// counters, both directions
static volatile uint32_t gUsbToT1s = 0, gUsbToT1sDrop = 0, gT1sToUsb = 0, gT1sToUsbDrop = 0;
static volatile uint64_t gUsbToT1sBytes = 0, gT1sToUsbBytes = 0;

struct Frame { uint8_t *buf; uint16_t len; };
static QueueHandle_t gToT1s = nullptr, gToUsb = nullptr;   // 32 frames each

// ---------------------------------------------------------------- USB: CDC-NCM network function
// TinyUSB's NCM class is compiled into the Arduino core (CONFIG_TINYUSB_NCM_ENABLED); it needs a
// descriptor, a MAC address for the host side, and the three callbacks below.
extern "C" {
uint8_t tud_network_mac_address[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};   // set from efuse in setup()
}

static uint16_t ncmDescriptor(uint8_t *dst, uint8_t *itf) {
  static char macStr[13];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", tud_network_mac_address[0],
           tud_network_mac_address[1], tud_network_mac_address[2], tud_network_mac_address[3],
           tud_network_mac_address[4], tud_network_mac_address[5]);
  const uint8_t strIdx = tinyusb_add_string_descriptor("10BASE-T1S (LAN8651)");
  const uint8_t macIdx = tinyusb_add_string_descriptor(macStr);
  const uint8_t epNotif = tinyusb_get_free_in_endpoint();
  const uint8_t epIn = tinyusb_get_free_in_endpoint();
  const uint8_t epOut = tinyusb_get_free_out_endpoint();
  uint8_t d[TUD_CDC_NCM_DESC_LEN] = {TUD_CDC_NCM_DESCRIPTOR(*itf, strIdx, macIdx, (uint8_t)(0x80 | epNotif), 64, epOut,
                                                           (uint8_t)(0x80 | epIn), 64, CFG_TUD_NET_MTU)};
  *itf += 2;
  memcpy(dst, d, sizeof(d));
  return sizeof(d);
}

// Registered from a global constructor: Arduino starts USB before setup() when CDC is on at
// boot, and interfaces must be known before that (the core's own USBCDC does the same).
static struct NcmRegistration {
  NcmRegistration() {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_ETH);
    m[0] = (m[0] | 0x02) & 0xFE;   // locally administered, unicast: the computer's side
    m[5] ^= 0x55;                  // and distinct from the LAN8651's own (efuse) address
    memcpy(tud_network_mac_address, m, 6);
    tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_NCM_DESC_LEN, ncmDescriptor);
  }
} gNcmRegistration;

extern "C" bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
  // computer -> bus. Copied and handed to a task: the LAN8651 transmit takes SPI time that the
  // USB task should not spend.
  uint8_t *b = (uint8_t *)malloc(size);
  Frame f = {b, size};
  if (b) memcpy(b, src, size);
  if (!b || xQueueSend(gToT1s, &f, 0) != pdTRUE) {
    free(b);
    gUsbToT1sDrop = gUsbToT1sDrop + 1;
  }
  tud_network_recv_renew();
  return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
  memcpy(dst, ref, arg);
  return arg;
}

extern "C" void tud_network_init_cb(void) {}

static void toT1sTask(void *) {
  Frame f;
  for (;;) {
    if (xQueueReceive(gToT1s, &f, portMAX_DELAY) != pdTRUE) continue;
    if (gEth && esp_eth_transmit(gEth, f.buf, f.len) == ESP_OK) {
      gUsbToT1s = gUsbToT1s + 1;
      gUsbToT1sBytes = gUsbToT1sBytes + f.len;
    } else gUsbToT1sDrop = gUsbToT1sDrop + 1;
    free(f.buf);
  }
}

static void toUsbTask(void *) {
  Frame f;
  for (;;) {
    if (xQueueReceive(gToUsb, &f, portMAX_DELAY) != pdTRUE) continue;
    // Wait for the IN buffer (the core builds TinyUSB with one NTB, so it is busy for the whole
    // ~1.5 ms a full frame takes at USB full speed). Poll every 50 us, not every 1 ms tick: a
    // tick per frame capped bus -> computer near 4 Mbit/s. The USB task runs at the top priority,
    // so this loop cannot hold it off. A host that stopped reading costs at most 50 ms per frame.
    const int64_t t0 = esp_timer_get_time();
    bool ok;
    while (!(ok = tud_network_can_xmit(f.len)) && esp_timer_get_time() - t0 < 50000) {
      esp_rom_delay_us(50);
      taskYIELD();
    }
    if (ok) {
      tud_network_xmit(f.buf, f.len);
      gT1sToUsb = gT1sToUsb + 1;
      gT1sToUsbBytes = gT1sToUsbBytes + f.len;
    } else gT1sToUsbDrop = gT1sToUsbDrop + 1;
    free(f.buf);
  }
}

// bus -> computer: the driver hands over a malloc'd frame that we now own
static esp_err_t onT1sFrame(esp_eth_handle_t, uint8_t *buf, uint32_t len, void *) {
  Frame f = {buf, (uint16_t)len};
  if (!tud_ready() || xQueueSend(gToUsb, &f, 0) != pdTRUE) {
    free(buf);
    gT1sToUsbDrop = gT1sToUsbDrop + 1;
  }
  return ESP_OK;
}

// ---------------------------------------------------------------- LAN8651

static void onEthEvent(void *, esp_event_base_t, int32_t id, void *) {
  if (id != ETHERNET_EVENT_CONNECTED && id != ETHERNET_EVENT_DISCONNECTED) return;
  gLinkUp = id == ETHERNET_EVENT_CONNECTED;
  tud_network_link_state(0, gLinkUp);
  Serial.printf("t1s: link %s\n", gLinkUp ? "up" : "down");
}

static esp_err_t applyPlca() {
  bool en = gPlcaId != kPlcaOff;
  esp_err_t err;
  if (en) {
    uint8_t id = gPlcaId, cnt = gPlcaCount;
    if ((err = esp_eth_ioctl(gEth, (esp_eth_io_cmd_t)LAN86XX_ETH_CMD_S_PLCA_ID, &id)) != ESP_OK) return err;
    if ((err = esp_eth_ioctl(gEth, (esp_eth_io_cmd_t)LAN86XX_ETH_CMD_S_PLCA_NCNT, &cnt)) != ESP_OK) return err;
  }
  return esp_eth_ioctl(gEth, (esp_eth_io_cmd_t)LAN86XX_ETH_CMD_S_EN_PLCA, &en);
}

static void printPlca() {
  bool en = false;
  uint8_t id = 0, cnt = 0;
  esp_eth_ioctl(gEth, (esp_eth_io_cmd_t)LAN86XX_ETH_CMD_G_EN_PLCA, &en);
  esp_eth_ioctl(gEth, (esp_eth_io_cmd_t)LAN86XX_ETH_CMD_G_PLCA_ID, &id);
  esp_eth_ioctl(gEth, (esp_eth_io_cmd_t)LAN86XX_ETH_CMD_G_PLCA_NCNT, &cnt);
  uint32_t pst = 0;
  if (en) esp_eth_mac_lan865x_read_reg(gMac, 4, 0xCA03, &pst);
  if (en) Serial.printf("plca: on, id %u of %u%s, %s\n", id, cnt, id == 0 ? " (coordinator)" : "",
                        (pst & 0x8000) ? "beacons seen" : "NO beacons");
  else Serial.println("plca: off (CSMA/CD)");
}

static bool t1sStart() {
  pinMode(kPinSdCs, OUTPUT);            // the TF card shares the bus: keep it deselected
  digitalWrite(kPinSdCs, HIGH);
  pinMode(kPinT1sReset, OUTPUT);
  digitalWrite(kPinT1sReset, LOW);
  delay(2);
  digitalWrite(kPinT1sReset, HIGH);
  delay(10);
  esp_event_loop_create_default();
  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, onEthEvent, nullptr);
  gpio_install_isr_service(0);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = kPinT1sMosi;
  bus.miso_io_num = kPinT1sMiso;
  bus.sclk_io_num = kPinT1sSclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
  for (int pin : {kPinT1sSclk, kPinT1sMosi, kPinT1sCs}) gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_1);

  int hz = 25 * 1000 * 1000;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  while (hz > 1000000 && spi_get_actual_clock(80 * 1000 * 1000, hz, 128) > kSpiMaxHz) hz -= 500000;
  hz = spi_get_actual_clock(80 * 1000 * 1000, hz, 128);
#pragma GCC diagnostic pop
  static spi_device_interface_config_t dev = {};
  dev.mode = 0;
  dev.clock_speed_hz = hz;
  dev.spics_io_num = kPinT1sCs;
  dev.queue_size = 20;
  eth_lan865x_config_t lanCfg = {};
  lanCfg.spi_host_id = SPI3_HOST;
  lanCfg.spi_devcfg = &dev;
  lanCfg.int_gpio_num = kPinT1sIrq;
  lanCfg.poll_period_ms = 0;
  lanCfg.custom_spi_driver = ETH_DEFAULT_SPI;
  eth_mac_config_t macCfg = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phyCfg = ETH_PHY_DEFAULT_CONFIG();
  phyCfg.reset_gpio_num = -1;
  gMac = esp_eth_mac_new_lan865x(&lanCfg, &macCfg);
  esp_eth_phy_t *phy = esp_eth_phy_new_lan865x(&phyCfg);
  if (!gMac || !phy) return false;
  esp_eth_config_t ethCfg = ETH_DEFAULT_CONFIG(gMac, phy);
  esp_err_t err = esp_eth_driver_install(&ethCfg, &gEth);
  Serial.printf("t1s: driver install: %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;
  gSpiMhz = hz / 1e6f;
  gpio_pulldown_dis((gpio_num_t)kPinT1sIrq);   // the HAT has its own 10k pull-up on IRQ_N
  gpio_pullup_en((gpio_num_t)kPinT1sIrq);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_ETH);
  esp_eth_ioctl(gEth, ETH_CMD_S_MAC_ADDR, mac);   // only for pause frames; data keeps the host's source
  bool on = true;
  esp_eth_ioctl(gEth, ETH_CMD_S_PROMISCUOUS, &on);   // every frame on the bus goes to the computer
  esp_eth_update_input_path(gEth, onT1sFrame, nullptr);
  Serial.printf("t1s: plca config: %s\n", esp_err_to_name(applyPlca()));
  err = esp_eth_start(gEth);
  Serial.printf("t1s: start: %s, SPI %.2f MHz\n", esp_err_to_name(err), gSpiMhz);
  return err == ESP_OK;
}

// ---------------------------------------------------------------- console

static void status() {
  Serial.printf("t1s: link %s, SPI %.2f MHz\n", gLinkUp ? "up" : "down", gSpiMhz);
  if (gEth) printPlca();
  Serial.printf("usb: %s, host-side mac %02x:%02x:%02x:%02x:%02x:%02x\n", tud_ready() ? "configured" : "not configured",
                tud_network_mac_address[0], tud_network_mac_address[1], tud_network_mac_address[2],
                tud_network_mac_address[3], tud_network_mac_address[4], tud_network_mac_address[5]);
  Serial.printf("usb -> t1s: %lu frames, %llu B, %lu dropped\n", (unsigned long)gUsbToT1s,
                (unsigned long long)gUsbToT1sBytes, (unsigned long)gUsbToT1sDrop);
  Serial.printf("t1s -> usb: %lu frames, %llu B, %lu dropped\n", (unsigned long)gT1sToUsb,
                (unsigned long long)gT1sToUsbBytes, (unsigned long)gT1sToUsbDrop);
  Serial.printf("heap: %lu B free\n", (unsigned long)ESP.getFreeHeap());
}

static void help() {
  Serial.println(
      "status                 link, PLCA as read back, frame counters both ways\n"
      "plca <id> [count]      PLCA node id (0 = coordinator) and node count, applied now\n"
      "csma                   PLCA off, plain CSMA/CD\n"
      "save / reboot          keep the PLCA setting / restart");
}

static void handleLine(char *line) {
  char cmd[16] = {};
  int a = -1, b = -1;
  if (sscanf(line, "%15s %d %d", cmd, &a, &b) < 1) return;
  if (!strcmp(cmd, "status")) status();
  else if (!strcmp(cmd, "plca") && a >= 0) {
    gPlcaId = a;
    if (b > 0) gPlcaCount = b;
    Serial.printf("plca: %s\n", esp_err_to_name(applyPlca()));
    printPlca();
  } else if (!strcmp(cmd, "csma")) {
    gPlcaId = kPlcaOff;
    Serial.printf("plca: %s\n", esp_err_to_name(applyPlca()));
    printPlca();
  } else if (!strcmp(cmd, "save")) {
    gPrefs.begin("t1s", false);
    gPrefs.putUChar("id", gPlcaId);
    gPrefs.putUChar("cnt", gPlcaCount);
    gPrefs.end();
    Serial.println("saved");
  } else if (!strcmp(cmd, "reboot")) ESP.restart();
  else help();
}

void setup() {
  Serial.begin(115200);
  pinMode(kPinBoardLed, OUTPUT);
  gPrefs.begin("t1s", true);
  gPlcaId = gPrefs.getUChar("id", gPlcaId);
  gPlcaCount = gPrefs.getUChar("cnt", gPlcaCount);
  gPrefs.end();
  gToT1s = xQueueCreate(32, sizeof(Frame));
  gToUsb = xQueueCreate(32, sizeof(Frame));
  xTaskCreatePinnedToCore(toT1sTask, "to_t1s", 4096, nullptr, 6, nullptr, 1);
  xTaskCreatePinnedToCore(toUsbTask, "to_usb", 4096, nullptr, 6, nullptr, 1);
  delay(1500);   // let the CDC console enumerate so the bring-up log is seen
  Serial.println("\n== t1s_usb_nic: USB (CDC-NCM) <-> LAN8651 10BASE-T1S ==");
  if (!t1sStart()) Serial.println("t1s: bring-up FAILED (HAT seated? CS on IO0, MOSI IO11, MISO IO9)");
  status();
  help();
}

void loop() {
  static char line[64];
  static size_t len = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { line[len] = 0; handleLine(line); len = 0; }
    else if (len < sizeof(line) - 1) line[len++] = c;
  }
  // LED: solid = PLCA beacons seen (or link up with PLCA off), fast blink = no beacons, slow = no link
  static uint32_t tPst = 0;
  static bool pst = false;
  if (gPlcaId != kPlcaOff && gMac && millis() - tPst > 250) {
    tPst = millis();
    uint32_t v = 0;
    if (esp_eth_mac_lan865x_read_reg(gMac, 4, 0xCA03, &v) == ESP_OK) pst = v & 0x8000;
  }
  bool on = !gLinkUp ? (millis() / 500) & 1 : gPlcaId == kPlcaOff ? true : pst || ((millis() / 125) & 1);
  digitalWrite(kPinBoardLed, on);
  delay(5);
}
