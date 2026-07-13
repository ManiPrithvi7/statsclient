#ifndef DWIN_PROTOCOL_H
#define DWIN_PROTOCOL_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t dwin_init(void);
void dwin_deinit(void);
void dwin_set_page(uint16_t page_id);
void dwin_write_word(uint16_t addr, uint16_t value);
void dwin_send_qr(uint16_t vp, const char *txt, size_t max_url_bytes);

#endif /* DWIN_PROTOCOL_H */
