/* J1 DW-MMC PIO PAL (Exynos3475 SDMMC @0x13560000, emulator dw-mmc).
 * Read-only PIO CMD17/18/12 + init CMD0/8/55/41/2/3/9/7/6. Numeric offsets
 * are factual DW-MMC data. No foreign code imported. */
#ifndef IOS7_J1_SDMMC_H
#define IOS7_J1_SDMMC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define J1_SDMMC_PA 0x13560000U
#define J1_SD_OK 0
#define J1_SD_TIMEOUT -1
#define J1_SD_CRC -2
#define J1_SD_ARG -3
struct j1_sd_io {
    void *cookie;
    uint32_t (*read)(void *, unsigned reg);
    void (*write)(void *, unsigned reg, uint32_t v);
    int (*delay_us)(void *, uint32_t us);
    uint64_t (*now_ns)(void *, int dummy);
    uint32_t poll_bound;
};
struct j1_sd_card {
    struct j1_sd_io io;
    uint32_t rca, ocr, cid[4], csd[4];
    uint64_t sectors;
    int sdhc;
    uint32_t command, status;
};
void j1_sd_init(struct j1_sd_card *c, const struct j1_sd_io *io);
int j1_sd_probe(struct j1_sd_card *c);
int j1_sd_read512(struct j1_sd_card *c, uint64_t lba, uint8_t out[512]);
int j1_sd_read_multi(struct j1_sd_card *c, uint64_t lba, uint8_t *out, uint32_t nsectors);
#ifdef __cplusplus
}
#endif
#endif
