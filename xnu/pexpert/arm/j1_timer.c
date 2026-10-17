/* J1 Exynos3475 single-core XNU IRQ/timebase: the SoC MCT global timer
 * (global free-running comparator 0) as the tick source, delivered through
 * the GIC on INTID 134 (SPI 102 + 32) on hardware and INTID 166 under the
 * QEMU j1xlte machine (see J1_MCT_IRQ).  The ARM generic timer (CP15 CNTP)
 * path was abandoned because the QEMU j1xlte machine does not wire PPI 30
 * to the GIC.  Register layouts are factual J1/Exynos data; no foreign code
 * imported.  The MCT count is 26 MHz on the phone (fin_pll); on the emulator
 * the stock exynos4210.mct runs at 24 MHz, which only scales wall-clock. */
#include <mach/mach_types.h>
#include <mach/arm/thread_status.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm/protos.h>
#include <machine/machine_routines.h>
#include <kern/debug.h>
#include "j1_timer.h"

#if defined(BOARD_CONFIG_EXYNOS3475_J1XLTE)
#define J1_MCT_PA 0x100B0000U
#define J1_GIC_DIST_PA 0x10C01000U
#define J1_GIC_CPU_PA 0x10C02000U
#define J1_IO_BYTES 0x1000U

/* MCT global block (factual). */
#define J1_MCT_G_CTL 0x010U
#define J1_MCT_G_CNT_L 0x100U
#define J1_MCT_G_CNT_U 0x104U
#define J1_MCT_G_CNT_WSTAT 0x110U
#define J1_MCT_G_COMP0_L 0x200U
#define J1_MCT_G_COMP0_U 0x204U
#define J1_MCT_G_COMP0_ADD_INCR 0x208U
#define J1_MCT_G_TCON 0x240U
#define J1_MCT_G_INT_CSTAT 0x244U
#define J1_MCT_G_INT_ENB 0x248U
#define J1_MCT_G_WSTAT 0x24CU

#define J1_MCT_CTL_ENABLE 0x1U
#define J1_MCT_TCON_COMP0_ENABLE (1U << 0)
#define J1_MCT_TCON_COMP0_AUTO_INC (1U << 1)
#define J1_MCT_TCON_START (1U << 8)
#define J1_MCT_WSTAT_COMP0_L (1U << 0)
#define J1_MCT_WSTAT_COMP0_U (1U << 1)
#define J1_MCT_WSTAT_COMP0_INCR (1U << 2)
#define J1_MCT_WSTAT_TCON (1U << 16)

/* GIC-400 distributor / CPU interface (factual). */
#define J1_GIC_DIST_CTRL 0x000U
#define J1_GIC_DIST_ENABLE_SET 0x100U
#define J1_GIC_DIST_ENABLE_CLEAR 0x180U
#define J1_GIC_DIST_PENDING_CLEAR 0x280U
#define J1_GIC_DIST_PRI 0x400U
#define J1_GIC_DIST_TARGET 0x800U
#define J1_GIC_DIST_CONFIG 0xc00U
#define J1_GIC_CPU_CTRL 0x00U
#define J1_GIC_CPU_PRIMASK 0x04U
#define J1_GIC_CPU_INTACK 0x0cU
#define J1_GIC_CPU_EOI 0x10U

#define J1_TIMER_IRQ J1_MCT_IRQ

extern void rtc_configure(uint64_t hz);
extern void rtclock_intr(arm_saved_state_t *regs);
extern void ios7j1_timer_observe(uint32_t raw_ticks, uint32_t actual_irq_count);
extern void ios7j1_debug_putc(char c);

uint64_t clock_decrementer;
static vm_offset_t j1_mct, j1_gic_dist, j1_gic_cpu;
static uint32_t controller_ready, timebase_ready, counter_observed, armed;
static uint32_t last_count_lo, last_match_lo, irq_count, last_irq_counter, last_vector;
static uint32_t spurious_irq_count;
static uint32_t timer_hz = J1_MCT_HZ;

static uint32_t read_reg(vm_offset_t base, uint32_t off)
{ return *(volatile uint32_t *)(base+off); }
static void write_reg(vm_offset_t base, uint32_t off, uint32_t v)
{ *(volatile uint32_t *)(base+off)=v; }
static void drain(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* MCT register write with the WSTAT handshake (finish previous write, then
 * clear the status bit).  Same protocol as the J1XLTE reference. */
static void mct_write(uint32_t value, uint32_t offset, uint32_t wstat_mask)
{
    uint32_t i;
    write_reg(j1_mct, offset, value);
    /* Set-then-clear protocol, same as the Linux reference. A write that
     * never synchronizes used to be silently ignored (stale comparator,
     * silent timer death); fail loudly like Linux instead. */
    for (i = 0; i < 0x10000U; i++) {
        if (read_reg(j1_mct, J1_MCT_G_WSTAT) & wstat_mask) {
            write_reg(j1_mct, J1_MCT_G_WSTAT, wstat_mask);
            return;
        }
    }
    panic("J1 MCT write stuck: value=0x%x offset=0x%x mask=0x%x", value, offset, wstat_mask);
}

static uint64_t mct_get_frc(void)
{
    uint32_t lo, hi, hi2;
    do {
        hi = read_reg(j1_mct, J1_MCT_G_CNT_U);
        lo = read_reg(j1_mct, J1_MCT_G_CNT_L);
        hi2 = read_reg(j1_mct, J1_MCT_G_CNT_U);
    } while (hi != hi2);
    return ((uint64_t)hi2 << 32) | lo;
}

static void mct_frc_start(void)
{
    uint32_t tcon = read_reg(j1_mct, J1_MCT_G_TCON);
    mct_write(tcon | J1_MCT_TCON_START, J1_MCT_G_TCON, J1_MCT_WSTAT_TCON);
}

static void j1_put_hex(uint32_t v)
{
    int q;
    for (q = 0; q < 8; q++) {
        unsigned int n = (v >> ((7-q)*4)) & 15U;
        ios7j1_debug_putc((char)(n < 10 ? ('0'+n) : ('a'+n-10)));
    }
}

void J1_interrupt_init(void)
{
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    uint32_t i, irq=J1_TIMER_IRQ;
    if (controller_ready) { ml_set_interrupts_enabled(was_enabled); return; }
    j1_gic_dist=ml_io_map(J1_GIC_DIST_PA,J1_IO_BYTES*4);
    j1_gic_cpu=ml_io_map(J1_GIC_CPU_PA,J1_IO_BYTES);
    if(!j1_gic_dist || !j1_gic_cpu) panic("J1 GIC IO mapping failed");
    for (i=0;i<8;i++) {
        write_reg(j1_gic_dist, J1_GIC_DIST_ENABLE_CLEAR + i*4, 0xffffffffU);
        write_reg(j1_gic_dist, J1_GIC_DIST_PENDING_CLEAR + i*4, 0xffffffffU);
    }
    /* MCT global comparator 0 line (J1_MCT_IRQ): mid priority, level
     * triggered, routed to CPU0.  The target write is a no-op on the
     * uniprocessor GIC (RAZ/WI) but required on real hardware. */
    write_reg(j1_gic_dist, J1_GIC_DIST_PRI + (irq/4)*4,
        (read_reg(j1_gic_dist, J1_GIC_DIST_PRI + (irq/4)*4)&~(0xffU<<((irq%4)*8)))|(0x80U<<((irq%4)*8)));
    write_reg(j1_gic_dist, J1_GIC_DIST_CONFIG + (irq/16)*4,
        read_reg(j1_gic_dist, J1_GIC_DIST_CONFIG + (irq/16)*4)&~(0x3U<<((irq%16)*2)));
    write_reg(j1_gic_dist, J1_GIC_DIST_TARGET + (irq/4)*4,
        (read_reg(j1_gic_dist, J1_GIC_DIST_TARGET + (irq/4)*4)&~(0xffU<<((irq%4)*8)))|(0x01U<<((irq%4)*8)));
    write_reg(j1_gic_dist, J1_GIC_DIST_CTRL, 1U);
    write_reg(j1_gic_cpu, J1_GIC_CPU_PRIMASK, 0xf0U);
    write_reg(j1_gic_cpu, J1_GIC_CPU_CTRL, 1U);
    drain();
    controller_ready=1;
    {
        unsigned int v[3];
        v[0]=read_reg(j1_gic_dist, J1_GIC_DIST_CTRL);
        v[1]=read_reg(j1_gic_cpu, J1_GIC_CPU_CTRL);
        v[2]=read_reg(j1_gic_dist, J1_GIC_DIST_TARGET + (irq/4)*4);
        ios7j1_debug_putc('G'); ios7j1_debug_putc('I'); ios7j1_debug_putc('C');
        ios7j1_debug_putc(' ');
        j1_put_hex(v[0]); ios7j1_debug_putc(' ');
        j1_put_hex(v[1]); ios7j1_debug_putc(' ');
        j1_put_hex(v[2]);
        ios7j1_debug_putc('\n');
    }
    ml_set_interrupts_enabled(was_enabled);
}

void J1_timebase_init(void)
{
    uint64_t before, after;
    uint32_t i;
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    if(!controller_ready) panic("J1 timebase before GIC initialization");
    if(timebase_ready) { ml_set_interrupts_enabled(was_enabled); return; }
    j1_mct=ml_io_map(J1_MCT_PA,J1_IO_BYTES);
    if(!j1_mct) panic("J1 MCT IO mapping failed");

    /* Open the MCT block clock, then let the global free-running counter run. */
    write_reg(j1_mct, J1_MCT_G_CTL, J1_MCT_CTL_ENABLE);
    mct_frc_start();
    write_reg(j1_mct, J1_MCT_G_INT_ENB, 0); /* comparator int off while arming */
    drain();

    before=mct_get_frc(); after=before;
    for(i=0;i<65536U;i++){ after=mct_get_frc(); if(after!=before) break; }
    if(after==before) panic("J1 MCT counter did not advance");
    counter_observed=1; last_count_lo=(uint32_t)after;
    timebase_ready=1;
    clock_decrementer=J1_MCT_HZ/100U; /* 10ms scheduler tick. */
    if(clock_decrementer==0) clock_decrementer=J1_MCT_HZ/100U;
    if(clock_decrementer<J1_MCT_MIN_MATCH_TICKS) clock_decrementer=J1_MCT_MIN_MATCH_TICKS;
    gPEClockFrequencyInfo.timebase_frequency_hz=timer_hz;
    gPEClockFrequencyInfo.dec_clock_rate_hz=timer_hz;
    rtc_configure(timer_hz);
    {
        unsigned int v[4];
        v[0]=read_reg(j1_mct, J1_MCT_G_CTL);
        v[1]=read_reg(j1_mct, J1_MCT_G_TCON);
        v[2]=read_reg(j1_mct, J1_MCT_G_INT_ENB);
        v[3]=read_reg(j1_mct, J1_MCT_G_INT_CSTAT);
        ios7j1_debug_putc('M'); ios7j1_debug_putc('C'); ios7j1_debug_putc('T');
        ios7j1_debug_putc(' ');
        for(i=0;i<4;i++){ j1_put_hex(v[i]); ios7j1_debug_putc(i==3?'\n':' '); }
    }
    J1_timer_enabled(TRUE);
    ml_set_interrupts_enabled(was_enabled);
}

void J1_timer_enabled(int enable)
{
    uint32_t ticks;
    uint64_t requested, frc, match;
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    uint32_t irq=J1_TIMER_IRQ;
    if(!timebase_ready) panic("J1 timer before timebase initialization");
    requested=clock_decrementer;
    if(!enable || requested==~0ULL){
        write_reg(j1_mct, J1_MCT_G_INT_ENB, 0);
        write_reg(j1_gic_dist, J1_GIC_DIST_ENABLE_CLEAR + (irq/32)*4, 1U<<(irq%32));
        armed=0;
        ml_set_interrupts_enabled(was_enabled); return;
    }
    write_reg(j1_mct, J1_MCT_G_INT_ENB, 0); /* disarm while programming */
    write_reg(j1_mct, J1_MCT_G_INT_CSTAT, 1U);
    write_reg(j1_gic_dist, J1_GIC_DIST_PENDING_CLEAR + (irq/32)*4, 1U<<(irq%32));
    drain();
    ticks=requested>0x7fffffffU?0x7fffffffU:(uint32_t)requested;
    if(ticks<J1_MCT_MIN_MATCH_TICKS) ticks=J1_MCT_MIN_MATCH_TICKS;
    frc=mct_get_frc();
    match=frc+ticks;
    mct_write((uint32_t)(match & 0xffffffffU), J1_MCT_G_COMP0_L, J1_MCT_WSTAT_COMP0_L);
    mct_write((uint32_t)(match >> 32), J1_MCT_G_COMP0_U, J1_MCT_WSTAT_COMP0_U);
    mct_write(ticks, J1_MCT_G_COMP0_ADD_INCR, J1_MCT_WSTAT_COMP0_INCR);
    mct_write(J1_MCT_TCON_COMP0_ENABLE | J1_MCT_TCON_COMP0_AUTO_INC | J1_MCT_TCON_START,
        J1_MCT_G_TCON, J1_MCT_WSTAT_TCON);
    write_reg(j1_mct, J1_MCT_G_INT_ENB, 1); /* comparator 0 interrupt on */
    write_reg(j1_gic_dist, J1_GIC_DIST_CTRL, 1U);
    write_reg(j1_gic_cpu, J1_GIC_CPU_CTRL, 1U);
    write_reg(j1_gic_dist, J1_GIC_DIST_ENABLE_SET + (irq/32)*4, 1U<<(irq%32));
    drain();
    armed=1;
    last_match_lo=(uint32_t)(match & 0xffffffffU);
    ml_set_interrupts_enabled(was_enabled);
}

uint64_t J1_get_timebase(void)
{
    if(!timebase_ready) return 0;
    return mct_get_frc();
}
uint64_t J1_timer_value(void)
{
    boolean_t w=ml_set_interrupts_enabled(FALSE);
    int64_t rem=0;
    if(timebase_ready && armed) {
        uint64_t now=mct_get_frc();
        uint64_t match=((uint64_t)read_reg(j1_mct, J1_MCT_G_COMP0_U)<<32)|
            read_reg(j1_mct, J1_MCT_G_COMP0_L);
        rem=(int64_t)(match-now);
    }
    ml_set_interrupts_enabled(w);
    return rem>0?(uint32_t)rem:0;
}
void J1_handle_interrupt(void *context)
{
    uint32_t ack, irq, budget;
    if(!controller_ready || !timebase_ready) panic("J1 IRQ before controller/timebase");
    for(budget=0;budget<64U;budget++){
        ack=read_reg(j1_gic_cpu,J1_GIC_CPU_INTACK);
        irq=ack&0x3ffU;
        last_vector=irq;
        if(irq>=1020U){ if(budget==0) spurious_irq_count++; return; }
        if(irq!=J1_TIMER_IRQ){ write_reg(j1_gic_cpu,J1_GIC_CPU_EOI,ack); panic("J1 unexpected IRQ %u",irq); }
        /* Clear the MCT comparator status BEFORE EOI: the line is level
         * asserted and stays high until INT_CSTAT is written. */
        write_reg(j1_mct, J1_MCT_G_INT_CSTAT, 1U);
        write_reg(j1_gic_cpu,J1_GIC_CPU_EOI,ack);
        drain();
        last_count_lo=(uint32_t)mct_get_frc(); irq_count++;
        last_irq_counter=last_count_lo;
#if defined(BOARD_CONFIG_EXYNOS3475_J1XLTE)
        {
            extern unsigned int cons_ops[], nconsops, cons_ops_index;
            static unsigned int j1_tick_n, j1_dirty_seen;
            j1_tick_n++;
            if(!j1_dirty_seen &&
               (cons_ops[0]==0 || nconsops!=2 || cons_ops_index!=1)) {
                unsigned int v[3]; int i, k;
                j1_dirty_seen=1;
                ios7j1_debug_putc('D'); ios7j1_debug_putc('I');
                ios7j1_debug_putc('R'); ios7j1_debug_putc('T');
                ios7j1_debug_putc('Y'); ios7j1_debug_putc('@');
                v[0]=j1_tick_n; v[1]=cons_ops[0]; v[2]=nconsops;
                for(k=0;k<3;k++){
                    for(i=0;i<8;i++){ unsigned int n=(v[k]>>((7-i)*4))&15U;
                        ios7j1_debug_putc((char)(n<10?('0'+n):('a'+n-10))); }
                    ios7j1_debug_putc(' ');
                }
                ios7j1_debug_putc('\n');
            } else if((j1_tick_n&0x3FFU)==0) {
                unsigned int q=j1_tick_n>>10; int i;
                extern void ios7j1_fast_putc(char);
                ios7j1_debug_putc('T'); ios7j1_debug_putc('K');
                ios7j1_fast_putc('T'); ios7j1_fast_putc('K');
                for(i=0;i<8;i++){ unsigned int n=(q>>((7-i)*4))&15U;
                    ios7j1_debug_putc((char)(n<10?('0'+n):('a'+n-10)));
                    ios7j1_fast_putc((char)(n<10?('0'+n):('a'+n-10))); }
                ios7j1_debug_putc('\n');
                ios7j1_fast_putc('\n');
            }
        }
#endif
        rtclock_intr((arm_saved_state_t *)context);
#if !CONFIG_TICKLESS
        J1_timer_enabled(TRUE);
#endif
        ios7j1_timer_observe(last_count_lo,irq_count);
    }
    panic("J1 GIC IRQ drain budget exceeded");
}
void J1_timer_snapshot(struct J1TimerSnapshot *out)
{
    boolean_t w=ml_set_interrupts_enabled(FALSE);
    out->version=1;
    out->valid_flags=(controller_ready?1U:0)|(timebase_ready?2U:0)|(armed?4U:0)|(counter_observed?8U:0);
    out->count_lo=timebase_ready?(uint32_t)mct_get_frc():0;
    out->match_lo=timebase_ready?read_reg(j1_mct,J1_MCT_G_COMP0_L):0;
    out->enable=timebase_ready?read_reg(j1_mct,J1_MCT_G_TCON):0;
    out->irq_status=timebase_ready?read_reg(j1_mct,J1_MCT_G_INT_CSTAT):0;
    out->irq_count=irq_count; out->last_irq_counter=last_irq_counter;
    out->last_vector=last_vector; out->spurious_irq_count=spurious_irq_count;
    out->frequency_hz=timer_hz; out->minimum_match_ticks=J1_MCT_MIN_MATCH_TICKS;
    out->extended_ticks=timebase_ready?mct_get_frc():0;
    ml_set_interrupts_enabled(w);
}
#endif
