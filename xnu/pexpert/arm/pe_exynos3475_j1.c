/* J1 Exynos3475 PE (Galaxy J1 2016). HD2-architecture adaptation.
 * Validates loader handoff, maps FB/UART, installs GIC/MCT dispatch,
 * provides direct-FB display backend (no MDP DMA) and fatal reporter.
 * Numeric addresses are factual J1 hardware data. */
#if defined(BOARD_CONFIG_EXYNOS3475_J1XLTE)
#include <mach/mach_types.h>
#include <mach/host_info.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm/boot.h>
#include <pexpert/arm/protos.h>
#include <pexpert/arm/j1_handoff.h>
#include <pexpert/arm/j1_entry.h>
#include "j1_timer.h"
#include <machine/machine_routines.h>
#include <kern/debug.h>
#include <kern/clock.h>
#include <mach/vm_param.h>

extern int switch_to_serial_console(void);
extern host_t host_self(void);
extern kern_return_t host_statistics64(host_t host, host_flavor_t flavor,
                                       host_info64_t info,
                                       mach_msg_type_number_t *count);
extern uint64_t sane_size;
extern unsigned int vm_page_free_count,vm_page_free_reserved,vm_page_free_target;

static volatile unsigned int *j1_pixels;
static vm_offset_t j1_fb_map;
static unsigned int j1_fb_physical;
static volatile unsigned int j1_graphics_owner,j1_fatal_active;
static unsigned int j1_stage;
static volatile unsigned int j1_console_count;
static volatile unsigned int j1_fatal_category;
static volatile unsigned int j1_fatal_state[8];

void ios7j1_stage(unsigned int n)
{
 (void)n;
}
static unsigned int j1_callsite(void *r){ return (((unsigned int)r)&~1U)-4U; }

int ios7j1_fatal_report(unsigned int cat, unsigned int cs,
 unsigned int pc,unsigned int lr,unsigned int far,unsigned int fsr,
 unsigned int mask,const char *msg)
{
 (void)cs;
 if(j1_fatal_active) return 0;
 j1_fatal_active=1;
 if(gPESocDispatch.uart_putc==ios7j1_debug_putc) (void)switch_to_serial_console();
 j1_fatal_category=cat;
 j1_fatal_state[0]=cat;j1_fatal_state[1]=j1_stage;
 j1_fatal_state[2]=mask&15U;j1_fatal_state[3]=pc;
 j1_fatal_state[4]=lr;j1_fatal_state[5]=far;j1_fatal_state[6]=fsr;
 return 1;
}
void __attribute__((noreturn)) ios7j1_final_boundary(unsigned int reason)
{
 if(reason) ios7j1_fatal_report(0xf000U|(reason&0xfffU),
  j1_callsite(__builtin_return_address(0)),0,0,0,0,0,"ENTRY GUARD");
 __asm__ volatile("cpsid if" ::: "memory");
 for(;;) __asm__ volatile("dsb sy\n\twfi" ::: "memory");
}
void ios7j1_forget_early_maps(void){ j1_pixels=0; j1_fb_map=0; }
/* UART S3C @0x13820000: TX-only debug putc (factual offsets).
 * Before SocSupport maps it, write the physical address directly
 * (early boot runs identity-mapped); after, use the mapping. */
#define J1_UART_PA 0x13820000U
static vm_offset_t j1_uart_map;
static void j1_uart_putc_phys(char c)
{
 volatile unsigned int *utxh=(volatile unsigned int *)(J1_UART_PA+0x20);
 *utxh=(unsigned int)(unsigned char)c;
}
static void j1_uart_putc_raw(char c)
{
 volatile unsigned int *utxh=(volatile unsigned int *)(j1_uart_map+0x20);
 *utxh=(unsigned int)(unsigned char)c;
}
void ios7j1_debug_putc(char c)
{
 if(j1_console_count<1024) j1_console_count++;
 if(j1_uart_map) j1_uart_putc_raw(c);
 else j1_uart_putc_phys(c);
}
static void j1_puts(const char *s)
{
 while(*s) ios7j1_debug_putc(*s++);
}
static void j1_puthex(unsigned int v)
{
 int i; char h[8];
 for(i=0;i<8;i++){ unsigned int n=(v>>((7-i)*4))&15U; h[i]=(char)(n<10?('0'+n):('a'+n-10)); }
 for(i=0;i<8;i++) ios7j1_debug_putc(h[i]);
}
/* Early fault reporter for bring-up (called from sleh_abort before any
 * handling that might itself fault). Lock-free, UART+FB only. */
static volatile unsigned int j1_flt_n;
static volatile unsigned int j1_flt_last_pc = 0xffffffffU;
void ios7j1_fault_report(unsigned int pc,unsigned int lr,unsigned int far,
                         unsigned int fsr,unsigned int cpsr,unsigned int reason)
{
 if(j1_flt_n>=16) {
  /* Past the detailed window: only report a NEW faulting pc, once, so an
   * infinite fault loop is diagnosable without flooding the console. */
  if(pc==j1_flt_last_pc) return;
  j1_flt_last_pc=pc;
  j1_puts("J1FLT+ pc=");
  j1_puthex(pc);
  j1_puts(" far=");
  j1_puthex(far);
  j1_puts(" fsr=");
  j1_puthex(fsr);
  j1_puts(" cpsr=");
  j1_puthex(cpsr);
  j1_puts(" reason=");
  j1_puthex(reason);
  j1_puts("\n");
  return;
 }
 j1_flt_n++;
 j1_puts("J1FLT n=");
 j1_puthex(j1_flt_n);
 j1_puts(" pc=");
 j1_puthex(pc);
 j1_puts(" lr=");
 j1_puthex(lr);
 j1_puts(" far=");
 j1_puthex(far);
 j1_puts(" fsr=");
 j1_puthex(fsr);
 j1_puts(" cpsr=");
 j1_puthex(cpsr);
 j1_puts(" reason=");
 j1_puthex(reason);
 j1_puts("\n");
}
void ios7j1_fault_regs(unsigned int r0,unsigned int r1,unsigned int r2,
                       unsigned int r3)
{
 if(j1_flt_n>=16) return;
 j1_puts("J1REGS r0=");
 j1_puthex(r0);
 j1_puts(" r1=");
 j1_puthex(r1);
 j1_puts(" r2=");
 j1_puthex(r2);
 j1_puts(" r3=");
 j1_puthex(r3);
 j1_puts("\n");
}
static int j1_getc(void){ return -1; }
static void j1_uart_init(void){ }
void ios7j1_kernel_continuing(void){ }
void ios7j1_timer_observe(unsigned int t,unsigned int c){ (void)t;(void)c; }
int ios7j1_panic_text_begin(unsigned int r,unsigned int k,unsigned int p){ (void)r;(void)k;(void)p; return 0; }
void ios7j1_panic_text_putc(char c){ (void)c; }
void ios7j1_panic_text_end(void){ }

/* Direct-FB backend: 480x800x32 BGRA CPU copy, no DMA (always completed). */
static uint64_t j1_graphics_seq=1;
/* Panel text console (fast_putc): active during boot, auto-disabled the moment
 * the GUI display driver presents its first frame so logs stop clobbering it. */
volatile int j1_panel_console_enabled=1;
int ios7j1_display_ready(void)
{
 return j1_pixels && !j1_fatal_active &&
  PE_state.video.v_width==480 && PE_state.video.v_height==800 &&
  PE_state.video.v_rowBytes==1920 && PE_state.video.v_depth==32;
}
int ios7j1_display_present(const uint8_t *px,uint32_t bytes,uint32_t row,
                           uint32_t fmt,uint64_t *seq)
{
 unsigned int y;
 if(!px||!seq) return 0;
 if(!(fmt==0x42475241U||fmt==0x52474241U)) return 0;
 if(row<1920U||bytes<1536000U) return 0;
 if(!ios7j1_display_ready()) return 0;
 for(y=0;y<800;y++){
  const uint8_t *src=px+(uint64_t)y*row;
  uint8_t *dst=(uint8_t *)j1_pixels+y*1920U;
  unsigned int i;
  for(i=0;i<1920U;i++) dst[i]=src[i];
 }
 *seq=j1_graphics_seq++;
 if(!j1_graphics_seq) j1_graphics_seq=1;
 j1_panel_console_enabled=0;
 return 1;
}
int ios7j1_display_completed(uint64_t s){ return s && s<j1_graphics_seq; }

static int j1_video_ok(const boot_args *a)
{
 unsigned long long end=(unsigned long long)a->Video.v_baseAddr+1536000U;
 unsigned long long ramend=(unsigned long long)a->physBase+a->memSize;
 return a->Revision==1 && a->Version==2 && a->virtBase==0x80000000U &&
  a->physBase==0x30000000U && a->memSize==0x25E00000U &&
  a->topOfKernelData>=a->physBase && !(a->topOfKernelData&0x3fffU) &&
  (unsigned long long)a->topOfKernelData+0x220000U<=ramend &&
  a->Video.v_width==480 && a->Video.v_height==800 &&
  a->Video.v_rowBytes==1920 && a->Video.v_depth==32 &&
  !(a->Video.v_baseAddr&3U) && a->Video.v_baseAddr==0x57E05000U &&
  end<=(unsigned long long)0x57E05000U+0x200000U;
}
void ios7j1_checkpoint(boot_args *a, unsigned int stage)
{
 if(j1_fatal_active) return;
 if(stage==6){
  if(!j1_video_ok(a)) ios7j1_final_boundary(1);
  j1_fb_physical=a->Video.v_baseAddr;
  j1_pixels=(volatile unsigned int *)j1_fb_physical;
  /* Earliest serial: point dispatch at our putc (physical UART)
   * so PE_early_puts from here on is visible on the emulator. */
  gPESocDispatch.uart_putc=ios7j1_debug_putc;
  gPESocDispatch.uart_getc=j1_getc;
  gPESocDispatch.uart_init=j1_uart_init;
  j1_puts("J1 stage6 video ok\n");
 }
 if(stage!=j1_stage+1U||stage<6||stage>10){
  if(!(stage==6&&j1_stage==0)) ios7j1_final_boundary(3);
 }
 j1_stage=stage;
 if(stage==7) j1_puts("J1 stage7\n");
 else if(stage==8) j1_puts("J1 stage8\n");
 else if(stage==9) j1_puts("J1 stage9\n");
 else if(stage==10) j1_puts("J1 stage10\n");
}
void PE_init_SocSupport_stub(void)
{
 DTEntry chosen;
 ios7lab_j1_handoff *h=0;
 unsigned int size=0;
 boot_args *args=(boot_args *)PE_state.bootArgs;
 vm_offset_t fb;
 static ios7lab_j1_handoff j1_default_handoff;
 if(!j1_video_ok(args)) ios7j1_final_boundary(4);
 /* Handoff via /chosen when the loader provides it; otherwise synthesize
  * defaults from boot_args (uniLoader passes physBase/video/top directly). */
 if(DTLookupEntry(0,"/chosen",&chosen)==kSuccess &&
    DTGetProperty(chosen,IOS7LAB_J1_DT_PROPERTY,(void **)&h,&size)==kSuccess &&
    size==128&&h&&h->magic==IOS7LAB_J1_MAGIC&&h->version==1&&
    h->bytes==128&&h->machine==0x3475U){
  /* loader-provided handoff accepted as-is */
 }else{
  j1_default_handoff.magic=IOS7LAB_J1_MAGIC; j1_default_handoff.version=1;
  j1_default_handoff.bytes=128; j1_default_handoff.flags=IOS7LAB_J1_FLAGS;
  j1_default_handoff.machine=0x3475U;
  j1_default_handoff.disp_physical=0x57E05000U;
  j1_default_handoff.dma_address=args->Video.v_baseAddr;
  j1_default_handoff.dma_stride=1920; j1_default_handoff.dma_xy=0;
  j1_default_handoff.width=480; j1_default_handoff.height=800;
  j1_default_handoff.row_bytes=1920; j1_default_handoff.pixel_format=0x42475241U;
  j1_default_handoff.framebuffer_bytes=1536000;
  j1_default_handoff.memory_base=args->physBase;
  j1_default_handoff.memory_bytes=args->memSize;
  j1_default_handoff.top_of_kernel_data=args->topOfKernelData;
  j1_default_handoff.checkpoint_count=5;
  h=&j1_default_handoff;
 }
 (void)h;
 fb=ml_io_map(j1_fb_physical,1536000);
 j1_uart_map=ml_io_map(J1_UART_PA,4096);
 if(!fb||!j1_uart_map) ios7j1_final_boundary(5);
 /* DATA integrity + translation self-check before IOKit matching. */
 {
  extern unsigned int cons_ops[], nconsops, cons_ops_index;
  unsigned int ttbr0, ttbr1, ttcr, l1e, l2e, pa;
  unsigned int *l1tab;
  j1_puts("J1DATA cons_ops[0]=");
  j1_puthex(cons_ops[0]);
  j1_puts(" [1]=");
  j1_puthex(cons_ops[2]);
  j1_puts(" n=");
  j1_puthex(nconsops);
  j1_puts(" idx=");
  j1_puthex(cons_ops_index);
  j1_puts("\n");
  __asm__ volatile("mrc p15,0,%0,c2,c0,0" : "=r"(ttbr0));
  __asm__ volatile("mrc p15,0,%0,c2,c0,1" : "=r"(ttbr1));
  __asm__ volatile("mrc p15,0,%0,c2,c0,2" : "=r"(ttcr));
  l1tab=(unsigned int *)((ttbr1&~0x3FFFU)-0x30000000U+0x80000000U);
  if((ttcr&7U)==0)
   l1tab=(unsigned int *)((ttbr0&~0x3FFFU)-0x30000000U+0x80000000U);
  l1e=l1tab[0x8066ece0U>>20];
  pa=0; l2e=0;
  if((l1e&3U)==1){
   unsigned int *l2=(unsigned int *)((l1e&~0x3FFU)-0x30000000U+0x80000000U);
   l2e=l2[(0x8066ece0U>>12)&0xFFU];
   if((l2e&3U)==2||(l2e&3U)==3)
    pa=(l2e&0xFFFFF000U)|(0x8066ece0U&0xFFFU);
  }else if((l1e&3U)==2){
   pa=(l1e&0xFFF00000U)|(0x8066ece0U&0xFFFFFU);
  }
  j1_puts("J1DATA ttbr0=");
  j1_puthex(ttbr0);
  j1_puts(" ttbr1=");
  j1_puthex(ttbr1);
  j1_puts(" ttcr=");
  j1_puthex(ttcr);
  j1_puts(" l1=");
  j1_puthex(l1e);
  j1_puts(" l2=");
  j1_puthex(l2e);
  j1_puts(" pa=");
  j1_puthex(pa);
  j1_puts("\n");
 }
 j1_pixels=(volatile unsigned int *)fb;
 j1_fb_map=fb;
 PE_state.video.v_baseAddr=fb;
 PE_state.video.v_rowBytes=1920;PE_state.video.v_width=480;
 PE_state.video.v_height=800;PE_state.video.v_depth=32;
 gPESocDispatch.uart_putc=ios7j1_debug_putc;
 gPESocDispatch.uart_getc=j1_getc;
 gPESocDispatch.uart_init=j1_uart_init;
 gPESocDispatch.interrupt_init=J1_interrupt_init;
 gPESocDispatch.timebase_init=J1_timebase_init;
 gPESocDispatch.handle_interrupt=J1_handle_interrupt;
 gPESocDispatch.get_timebase=J1_get_timebase;
 gPESocDispatch.timer_value=J1_timer_value;
 gPESocDispatch.timer_enabled=J1_timer_enabled;
 {
  PE_Video cv=PE_state.video;
  cv.v_baseAddr=j1_fb_physical|1U; cv.v_baseAddrHigh=0;
  cv.v_offset=0; cv.v_length=800U*1920U;
  cv.v_width=480; cv.v_height=800; cv.v_rowBytes=1920; cv.v_depth=32;
  cv.v_scale=kPEScaleFactor1x;
  initialize_screen(&cv,kPETextMode);
 }
 ios7j1_checkpoint(args,9);
 __asm__ volatile("" ::: "memory");
}

/* Fast panel console modeled on the iOS4 exynos_fb_putc: the stock
 * video_console is too slow on uncached FB (per-char cursor
 * read-modify-write plus full-frame bcopy scroll). This draws only SET
 * pixels (transparent background, no cursor), wraps at 480x800, and
 * CLEARS on full (no scroll copy). iso_font is LSB-first (X=0 is bit0),
 * same as vc_render_char. */
#define J1F_W 8U
#define J1F_H 16U
#define J1F_COLS 60U
#define J1F_ROWS 50U
#define J1F_FB_W 480U
#define J1F_FB_H 800U
static unsigned int j1f_cx, j1f_cy;
static int j1f_remapped, j1f_wiped;extern unsigned char iso_font[];
extern boolean_t pmap_initialized;
static void j1f_blank_line(volatile unsigned int *fb, unsigned int y)
{
    unsigned int x, yy;
    for (yy = 0; yy < 16; yy++) {
        volatile unsigned int *row = fb + (y + yy) * 480U;
        for (x = 0; x < 480; x++) row[x] = 0xFF000000U;
    }
}
void ios7j1_fast_putc(char c)
{
    volatile unsigned int *fb = j1_pixels;
    unsigned int x, y;
    unsigned char uc = (unsigned char)c;
    if (!j1_panel_console_enabled) return;
    /* The early ml_io_map done in PE_init_SocSupport does not survive
     * pmap_bootstrap's table rebuild: re-resolve once running on the
     * final tables, else output vanishes into a stale mapping. */
    if (!j1f_remapped && pmap_initialized) {
        vm_offset_t nfb = ml_io_map(j1_fb_physical, 1536000);
        if (nfb) { j1_pixels = (volatile unsigned int *)nfb; fb = j1_pixels; }
        j1f_remapped = 1;
    }
    if (!fb) return;
    if (!j1f_wiped) {
        unsigned int i, n = 480U * 800U;
        for (i = 0; i < n; i++) fb[i] = 0xFF000000U;
        j1f_wiped = 1;
        j1f_cx = 0; j1f_cy = 0;
    }
    if (c == '\n') {
        j1f_cx = 0; j1f_cy += 16;
        if (j1f_cy + 16 > 800) j1f_cy = 0;
        j1f_blank_line(fb, j1f_cy);
    }
    else if (c == '\r') { j1f_cx = 0; }
    else if (uc >= 32 && uc < 127) {
        unsigned char *g = &iso_font[uc * 16];
        for (y = 0; y < 16; y++) {
            unsigned char bits = g[y];
            volatile unsigned int *row =
                fb + (j1f_cy + y) * 480U + j1f_cx;
            for (x = 0; x < 8; x++)
                if (bits & (1U << x)) row[x] = 0xFFFFFFFFU;
        }
        j1f_cx += 8;
        if (j1f_cx + 8 > 480) {
            j1f_cx = 0; j1f_cy += 16;
            if (j1f_cy + 16 > 800) j1f_cy = 0;
            j1f_blank_line(fb, j1f_cy);
        }
    }
}
#endif
