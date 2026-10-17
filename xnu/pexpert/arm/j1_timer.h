/* J1 XNU timer API. Register layout notes only; no foreign code imported. */
#ifndef IOS7LAB_J1_TIMER_H
#define IOS7LAB_J1_TIMER_H
#include <mach/mach_types.h>
#define J1_MCT_HZ 26000000U
/* MCT global comparator 0 is SPI 102 -> INTID 134 (SPI + 32).  Hardware and
 * the QEMU j1xlte machine agree on this number: the machine connects the MCT
 * output to GIC input line J1_MCT_SPI (102), which the model maps to INTID
 * 134.  There is no emulator-only interrupt number. */
#define J1_MCT_IRQ 134U /* SPI 102 + 32. */
#define J1_MCT_WRITE_DELAY_TICKS 64U
#define J1_MCT_MIN_MATCH_TICKS (J1_MCT_WRITE_DELAY_TICKS+4U)
struct J1TimerSnapshot {
    uint32_t version, valid_flags, count_lo, match_lo, enable, irq_status;
    uint32_t irq_count, last_irq_counter, last_vector, spurious_irq_count;
    uint32_t frequency_hz, minimum_match_ticks;
    uint64_t extended_ticks;
};
typedef char J1TimerSnapshot_size[(sizeof(struct J1TimerSnapshot)==56)?1:-1];
void J1_interrupt_init(void);
void J1_timebase_init(void);
void J1_handle_interrupt(void *context);
uint64_t J1_get_timebase(void);
uint64_t J1_timer_value(void);
void J1_timer_enabled(int enable);
void J1_timer_snapshot(struct J1TimerSnapshot *out);
extern uint64_t clock_decrementer;
#endif
