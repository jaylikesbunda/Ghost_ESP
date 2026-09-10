/**
 * @file name_sniff.h
 * @brief Passive local-name sniffer (mDNS / LLMNR / SSDP / NetBIOS-NS)
 *
 * No spoofing, no MITM: joins the multicast groups every joined client can
 * already read and prints who announces what. Works in STA mode while
 * staying connected.
 *
 * Follows the scan_saver pattern: results auto-save to /mnt/ghostesp/scans
 * only when Settings > Capture & Location > Saving > Auto Save Scans is on.
 * Listening works regardless; saving is opportunistic.
 */

#ifndef NAME_SNIFF_H
#define NAME_SNIFF_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t name_sniff_start(const char *ip_filter);
void name_sniff_stop(void);
bool name_sniff_is_running(void);
const char *name_sniff_get_filter(void);
const char *name_sniff_get_path(void);

#ifdef __cplusplus
}
#endif

#endif // NAME_SNIFF_H
