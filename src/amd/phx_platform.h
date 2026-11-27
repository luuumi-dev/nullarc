#pragma once
#include <stdint.h>

static inline int phx_is_firebat_u6c(uint16_t vendor, uint16_t device, uint8_t revision,
                                    uint16_t subvendor, uint16_t subdevice) {
    return vendor == 0x1002 && device == 0x1900 && revision == 0xba &&
           subvendor == 0x2014 && subdevice == 0x8001;
}
