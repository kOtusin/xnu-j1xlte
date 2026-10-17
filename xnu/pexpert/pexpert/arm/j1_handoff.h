/* J1 iOS7 handoff ABI (Exynos 3475, Galaxy J1 2016).
 * Same 128-byte layout as Leo handoff for upper-layer reuse, but J1
 * constants and validation. No runtime or hardware success is implied.
 * Every word is little-endian uint32 on ARMv7; exact size is 128 bytes. */
#ifndef IOS7LAB_J1_HANDOFF_H
#define IOS7LAB_J1_HANDOFF_H
#define IOS7LAB_J1_MAGIC 0x4a314831U /* "J1H1" */
#define IOS7LAB_J1_VERSION 1U
#define IOS7LAB_J1_HANDOFF_BYTES 128U
#define IOS7LAB_J1_DT_PROPERTY "ios7lab-j1-handoff"
#define IOS7LAB_J1_MACHINE 0x3475U /* Exynos 3475 */
#define IOS7LAB_J1_RGB565 0x35363552U
#define IOS7LAB_J1_FLAGS 3U
#define IOS7LAB_J1_SCTLR_REJECT 0x52080085U
/* J1 display: direct framebuffer RAM, no MDP DMA engine.
 * DISP_PHYSICAL is the FB RAM base itself; MAP covers FB + control. */
#define IOS7LAB_J1_DISP_PHYSICAL 0x57E05000U
#define IOS7LAB_J1_DISP_MAP_BYTES 0x00100000U
#define IOS7LAB_J1_FB_WIDTH 480U
#define IOS7LAB_J1_FB_HEIGHT 800U
#define IOS7LAB_J1_FB_ROW 1920U
#define IOS7LAB_J1_FB_BYTES 1536000U
#define IOS7LAB_J1_EARLY_VM_RESERVE 0x00220000U
#define IOS7LAB_J1_STAGE_BASE 0xe000U
/* J1 memory window (uniLoader j1xlte_defconfig, factual): */
#define IOS7LAB_J1_PHYS_BASE 0x30000000U
#define IOS7LAB_J1_MEM_SIZE 0x25E00000U
/* J1 MMIO (factual, reference only): */
#define IOS7LAB_J1_GIC_DIST 0x10C01000U
#define IOS7LAB_J1_GIC_CPU 0x10C02000U
#define IOS7LAB_J1_MCT_BASE 0x100B0000U
#define IOS7LAB_J1_UART_BASE 0x13820000U
#define IOS7LAB_J1_SDMMC_BASE 0x13560000U
typedef struct ios7lab_j1_handoff {
    unsigned int magic, version, bytes, flags;
    unsigned int machine, entry_sctlr, entry_cpsr, entry_atags;
    unsigned int disp_physical, dma_config, dma_size, dma_address;
    unsigned int dma_stride, dma_xy, width, height;
    unsigned int row_bytes, pixel_format, framebuffer_bytes, memory_base;
    unsigned int memory_bytes, kernel_base, kernel_extent, top_of_kernel_data;
    unsigned int loader_start, loader_end, initrd_start, initrd_end;
    unsigned int atags_end, checkpoint_count, reserved0, reserved1;
} ios7lab_j1_handoff;
#endif
