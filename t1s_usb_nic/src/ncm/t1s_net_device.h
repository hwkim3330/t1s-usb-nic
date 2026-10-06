// The renamed API of src/ncm/t1s_ncm_device.c (TinyUSB 0.18.0 class/net/net_device.h, MIT).
// Types and constants come from the original header; only the names differ.
#ifndef T1S_NET_DEVICE_H_
#define T1S_NET_DEVICE_H_

#include "tusb_option.h"
#include "device/usbd.h"
#include "class/net/net_device.h"

#ifdef __cplusplus
extern "C" {
#endif

void t1snet_recv_renew(void);
bool t1snet_can_xmit(uint16_t size);
void t1snet_xmit(void *ref, uint16_t arg);
bool t1snet_recv_cb(const uint8_t *src, uint16_t size);
uint16_t t1snet_xmit_cb(uint8_t *dst, void *ref, uint16_t arg);
void t1snet_init_cb(void);
extern uint8_t t1snet_mac_address[6];

void t1sncm_init(void);
bool t1sncm_deinit(void);
void t1sncm_reset(uint8_t rhport);
uint16_t t1sncm_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len);
bool t1sncm_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request);
bool t1sncm_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);

#ifdef __cplusplus
}
#endif
#endif
