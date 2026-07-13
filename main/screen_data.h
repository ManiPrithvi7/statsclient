#ifndef SCREEN_DATA_H
#define SCREEN_DATA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;
    uint32_t verified_review;
    float rating;
    int32_t remaining_goal;
    uint32_t next_goal;
    uint8_t progress;
    char qr_text[301];
} google_screen_data_t;

typedef struct {
    bool valid;
    uint32_t followers;
    uint32_t next_goal;
    int32_t remaining_goal;
    uint8_t progress;
    char qr_text[301];
} instagram_screen_data_t;

typedef enum {
    SCREEN_UPDATE_NONE = 0,
    SCREEN_UPDATE_GOOGLE,
    SCREEN_UPDATE_INSTAGRAM,
} screen_update_kind_t;

typedef struct {
    screen_update_kind_t kind;
    google_screen_data_t google;
    instagram_screen_data_t instagram;
} screen_update_t;

#endif /* SCREEN_DATA_H */
