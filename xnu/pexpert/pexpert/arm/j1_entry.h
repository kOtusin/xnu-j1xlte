#ifndef IOS7LAB_J1_ENTRY_H
#define IOS7LAB_J1_ENTRY_H
#include <pexpert/arm/boot.h>
void ios7j1_checkpoint(boot_args *, unsigned int);
void ios7j1_forget_early_maps(void);
void ios7j1_debug_putc(char);
void ios7j1_kernel_continuing(void);
void ios7j1_timer_observe(unsigned int raw_mct_ticks, unsigned int irq_count);
int ios7j1_panic_text_begin(unsigned int,unsigned int,unsigned int);
void ios7j1_panic_text_putc(char);
void ios7j1_panic_text_end(void);
int ios7j1_fatal_report(unsigned int category, unsigned int callsite,
                        unsigned int pc, unsigned int lr,
                        unsigned int far, unsigned int fsr,
                        unsigned int valid_mask, const char *message);
void ios7j1_final_boundary(unsigned int) __attribute__((noreturn));
#endif
