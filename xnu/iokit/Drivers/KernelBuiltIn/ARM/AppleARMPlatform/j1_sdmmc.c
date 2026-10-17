/* J1 DW-MMC PIO implementation. See header for contract.
 * Data path follows the DW 2.40a 64-bit FIFO discipline (STATUS FCNT,
 * RXDR-gated 64-bit pulls from the 0x200 data port). */
#include "j1_sdmmc.h"
#include <kern/debug.h>
#if defined(BOARD_CONFIG_EXYNOS3475_J1XLTE)
/* DW-MMC register offsets (factual): */
#define R_CTRL 0x00U
#define R_PWREN 0x04U
#define R_CLKDIV 0x08U
#define R_CLKENA 0x10U
#define R_TMOUT 0x14U
#define R_CTYPE 0x18U
#define R_BLKSIZ 0x1cU
#define R_BYTCNT 0x20U
#define R_CMDARG 0x28U
#define R_CMD 0x2cU
#define R_RESP0 0x30U
#define R_RESP1 0x34U
#define R_RESP2 0x38U
#define R_RESP3 0x3cU
#define R_RINT 0x44U
#define R_STATUS 0x48U
#define R_DATA 0x200U
#define R_DATA_U 0x204U
/* CTRL bits: */
#define CT_FIFO_RESET (1U<<1)
#define CT_DMA_RESET (1U<<2)
#define CT_INT_ENABLE (1U<<4)
/* CMD bits: */
#define C_START (1U<<31)
#define C_UPD_CLK (1U<<21)
#define C_DATA_EXP (1U<<9)
#define C_RESP_EXP (1U<<6)
#define C_RESP_LONG (1U<<7)
#define C_RESP_CRC (1U<<8)
#define C_PRV_DAT_WAIT (1U<<13)
#define C_STOP (1U<<14)
/* RINT bits: */
#define I_RESP_ERR (1U<<1)
#define I_CMD_DONE (1U<<2)
#define I_DATA_OVER (1U<<3)
#define I_RXDR (1U<<5)
#define I_RCRC (1U<<6)
#define I_RTO (1U<<8)
#define I_DTO (1U<<9)
#define I_ERR (I_RTO|I_RCRC|I_RESP_ERR)
#define FCNT(x) (((x)>>17)&0x1fffU)
static uint32_t rd(struct j1_sd_card *c, unsigned r){ return c->io.read(c->io.cookie,r); }
static void wr(struct j1_sd_card *c, unsigned r, uint32_t v){ c->io.write(c->io.cookie,r,v); }
static int cmd(struct j1_sd_card *c, uint32_t idx, uint32_t arg, uint32_t flags, uint32_t resp[4])
{
    uint32_t n=0; uint64_t t0=c->io.now_ns(c->io.cookie,0);
    wr(c,R_RINT,0xffffffffU);
    wr(c,R_CMDARG,arg);
    wr(c,R_CMD,C_START|(idx&0x3fU)|flags);
    while((rd(c,R_CMD)&C_START)){
        if(c->io.now_ns(c->io.cookie,0)-t0>1000000000ULL||++n>c->io.poll_bound) return J1_SD_TIMEOUT;
    }
    n=0; t0=c->io.now_ns(c->io.cookie,0);
    while(!(rd(c,R_RINT)&I_CMD_DONE)){
        uint32_t r=rd(c,R_RINT);
        if(r&I_RTO) return J1_SD_TIMEOUT;
        if(r&(I_RESP_ERR|I_RCRC)) return J1_SD_CRC;
        if(c->io.now_ns(c->io.cookie,0)-t0>1000000000ULL||++n>c->io.poll_bound) return J1_SD_TIMEOUT;
    }
    if(resp){ resp[0]=rd(c,R_RESP0); resp[1]=rd(c,R_RESP1); resp[2]=rd(c,R_RESP2); resp[3]=rd(c,R_RESP3); }
    wr(c,R_RINT,I_CMD_DONE);
    return J1_SD_OK;
}
void j1_sd_init(struct j1_sd_card *c, const struct j1_sd_io *io)
{
    unsigned i; uint8_t *p=(uint8_t*)c;
    for(i=0;i<sizeof(*c);i++)p[i]=0;
    c->io=*io;
}
static void j1_sd_tune(struct j1_sd_card *c);
int j1_sd_probe(struct j1_sd_card *c)
{
    uint32_t r[4]; int rc; uint32_t n=0;
    wr(c,R_PWREN,1U); wr(c,R_CLKDIV,0xffU); wr(c,R_CLKENA,1U);
    wr(c,R_TMOUT,0xffffffffU); wr(c,R_BLKSIZ,512U); wr(c,R_CTYPE,0U);
    rc=cmd(c,0,0,0,0); if(rc) return rc;
    rc=cmd(c,8,0x1aa,C_RESP_EXP, r); if(rc) return rc;
    if((r[0]&0xffU)!=0xaaU) return J1_SD_CRC;
    for(n=0;n<1000;n++){
        rc=cmd(c,55,0,C_RESP_EXP, r); if(rc) return rc;
        rc=cmd(c,41,0x40ff8000U,C_RESP_EXP, r); if(rc) return rc;
        if(r[0]&(1U<<31)) break;
    }
    c->ocr=r[0]; c->sdhc=(r[0]&(1U<<30))?1:0;
    rc=cmd(c,2,0,C_RESP_EXP|C_RESP_LONG, r); if(rc) return rc;
    c->cid[0]=r[0];c->cid[1]=r[1];c->cid[2]=r[2];c->cid[3]=r[3];
    rc=cmd(c,3,0,C_RESP_EXP, r); if(rc) return rc;
    c->rca=(r[0]>>16)&0xffffU;
    rc=cmd(c,9,c->rca<<16,C_RESP_EXP|C_RESP_LONG, r); if(rc) return rc;
    c->csd[0]=r[0];c->csd[1]=r[1];c->csd[2]=r[2];c->csd[3]=r[3];
    /* CSD v2 (SDHC): c_size @ bits 48..69. */
    {
        uint64_t csd_hi=((uint64_t)r[1]<<32)|r[0];
        uint32_t csize=(uint32_t)((csd_hi>>48)&0x3fffffULL);
        c->sectors=((uint64_t)csize+1U)*1024U;
    }
    rc=cmd(c,7,c->rca<<16,C_RESP_EXP, 0); if(rc) return rc;
    rc=cmd(c,55,c->rca<<16,C_RESP_EXP, r); if(rc) return rc;
    rc=cmd(c,6,2,C_RESP_EXP, r); if(rc) return rc;
    wr(c,R_CTYPE,1U); /* 4-bit */
    j1_sd_tune(c);
    return J1_SD_OK;
}
/* Clock update command (DW-MMC: apply CLKDIV/CLKENA changes). */
static void upd_clk(struct j1_sd_card *c)
{
    uint32_t t = 0;
    wr(c,R_CMDARG,0U);
    wr(c,R_CMD,C_START|C_UPD_CLK|C_PRV_DAT_WAIT);
    while((rd(c,R_CMD)&C_START) && t<1000000U) t++;
}
/* Raise the SD clock while single-block MBR reads verify, mirroring the
 * loader's sd_tune_clock (which settles at div 0x00 on this card). Stuck
 * at the 0xff identification speed, bulk phases (dyld shared cache,
 * ~300k single-block reads) take tens of minutes and look like hangs.
 * Every step is validated; on any failure the last good divider stays. */
static void j1_sd_tune(struct j1_sd_card *c)
{
    static const uint32_t divs[] = {
        0x7fU, 0x3fU, 0x1fU, 0x0fU, 0x07U, 0x03U, 0x01U, 0x00U,
    };
    uint8_t b[512];
    uint32_t good = 0xffU, i;
    for (i = 0; i < sizeof(divs)/sizeof(divs[0]); i++) {
        wr(c,R_CLKENA,0U); upd_clk(c);
        wr(c,R_CLKDIV,divs[i]); upd_clk(c);
        wr(c,R_CLKENA,1U); upd_clk(c);
        if (j1_sd_read512(c,0,b) != J1_SD_OK) break;
        if (b[510]!=0x55U || b[511]!=0xaaU) break;
        good = divs[i];
    }
    wr(c,R_CLKENA,0U); upd_clk(c);
    wr(c,R_CLKDIV,good); upd_clk(c);
    wr(c,R_CLKENA,1U); upd_clk(c);
    kprintf("J1SDMMC: clk-final div=%02x\n", good);
}
/* Drain words 64-bit entries (words = 8-byte units) from the data port. */
static int pio_drain(struct j1_sd_card *c, uint8_t *out, uint32_t words)
{
    uint32_t n=0; uint64_t t0=c->io.now_ns(c->io.cookie,0);
    uint32_t st=rd(c,R_RINT);
    while(!(st&(I_RXDR|I_DTO))){
        if(st&I_ERR) return J1_SD_CRC;
        if(c->io.now_ns(c->io.cookie,0)-t0>2000000000ULL||++n>c->io.poll_bound) return J1_SD_TIMEOUT;
        st=rd(c,R_RINT);
    }
    if(!(st&I_RXDR) && (st&I_DTO) && words==0) return J1_SD_OK;
    while(words>0){
        uint32_t fcnt=FCNT(rd(c,R_STATUS));
        uint32_t take, t1=0;
        if(fcnt==0){
            uint64_t t00=c->io.now_ns(c->io.cookie,0);
            while(((fcnt=FCNT(rd(c,R_STATUS)))==0)){
                if(c->io.now_ns(c->io.cookie,0)-t00>1000000000ULL||++t1>c->io.poll_bound) return J1_SD_TIMEOUT;
            }
        }
        take=(fcnt<words)?fcnt:words;
        while(take--){
            uint32_t lo=rd(c,R_DATA), hi=rd(c,R_DATA_U);
            out[0]=(uint8_t)lo; out[1]=(uint8_t)(lo>>8);
            out[2]=(uint8_t)(lo>>16); out[3]=(uint8_t)(lo>>24);
            out[4]=(uint8_t)hi; out[5]=(uint8_t)(hi>>8);
            out[6]=(uint8_t)(hi>>16); out[7]=(uint8_t)(hi>>24);
            out+=8; words--;
        }
    }
    return J1_SD_OK;
}
static int pio_wait_done(struct j1_sd_card *c)
{
    uint32_t n=0; uint64_t t0=c->io.now_ns(c->io.cookie,0);
    while(!(rd(c,R_RINT)&(I_DATA_OVER|I_ERR))){
        if(c->io.now_ns(c->io.cookie,0)-t0>2000000000ULL||++n>c->io.poll_bound) return J1_SD_TIMEOUT;
    }
    if(rd(c,R_RINT)&I_ERR) return J1_SD_CRC;
    wr(c,R_RINT,0xffffffffU);
    return J1_SD_OK;
}
static void data_reset(struct j1_sd_card *c)
{
    uint32_t v,n=0;
    wr(c,R_RINT,0xffffffffU);
    v=rd(c,R_CTRL);
    wr(c,R_CTRL,(v|CT_DMA_RESET|CT_FIFO_RESET)&~CT_INT_ENABLE);
    while((rd(c,R_CTRL)&(CT_DMA_RESET|CT_FIFO_RESET))){
        if(++n>100000U) break;
    }
    wr(c,R_CTRL,rd(c,R_CTRL)&~CT_INT_ENABLE);
}
int j1_sd_read_multi(struct j1_sd_card *c, uint64_t lba, uint8_t *out, uint32_t n)
{
    uint32_t r[4]; int rc;
    uint32_t arg=c->sdhc?(uint32_t)lba:(uint32_t)(lba*512U);
    data_reset(c);
    wr(c,R_BLKSIZ,512U); wr(c,R_BYTCNT,n*512U);
    wr(c,R_RINT,0xffffffffU);
    if(n==1){
        rc=cmd(c,17,arg,C_RESP_EXP|C_RESP_CRC|C_DATA_EXP|C_PRV_DAT_WAIT, r);
    }else{
        rc=cmd(c,18,arg,C_RESP_EXP|C_RESP_CRC|C_DATA_EXP|C_PRV_DAT_WAIT, r);
    }
    if(rc) return rc;
    rc=pio_drain(c,out,n*64U);
    if(rc) return rc;
    rc=pio_wait_done(c);
    if(rc) return rc;
    if(n>1){
        rc=cmd(c,12,0,C_STOP|C_RESP_EXP|C_RESP_CRC, 0);
        if(rc) return rc;
    }
    return J1_SD_OK;
}
int j1_sd_read512(struct j1_sd_card *c, uint64_t lba, uint8_t out[512])
{ return j1_sd_read_multi(c,lba,out,1); }
#endif
