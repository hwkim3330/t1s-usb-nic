// Hands the device stack our NCM class (t1s_ncm_device.c). usbd asks this before its built-in
// drivers, so this copy -- with three transmit NTBs -- claims the NCM interface.
#include "tusb_option.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "t1s_net_device.h"

static const usbd_class_driver_t t1s_ncm_driver = {
    .name = "T1S-NCM",
    .init = t1sncm_init,
    .deinit = t1sncm_deinit,
    .reset = t1sncm_reset,
    .open = t1sncm_open,
    .control_xfer_cb = t1sncm_control_xfer_cb,
    .xfer_cb = t1sncm_xfer_cb,
    .xfer_isr = NULL,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
  *driver_count = 1;
  return &t1s_ncm_driver;
}
