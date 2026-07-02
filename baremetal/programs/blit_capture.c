/* blit_capture — minimal looping dst+8 COPY for ILA capture on silicon.
 *
 * One unaligned (dst+8) COPY of a position-encoded source to a UNIQUE dst region
 * (0x0600_0008), repeated forever with a delay so the ILA can arm and trigger on
 * this blit's writes (app_addr in the 0x0600 range) without other blits in the way.
 * Each pass also does the invalidate + readback so the UART log confirms the
 * corruption is present on THIS (ILA) bitstream.
 */
#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"

#define BLIT_BASE        0xF00E0000u
#define BR(o)            (*(volatile uint64_t *)(unsigned long)(BLIT_BASE + (o)))
#define BLIT_CTRL        0x00u
#define BLIT_STATUS      0x08u
#define BLIT_DST_ADDR    0x10u
#define BLIT_DST_STRIDE  0x18u
#define BLIT_SRC_ADDR    0x20u
#define BLIT_SRC_STRIDE  0x28u
#define BLIT_WIDTH       0x30u
#define BLIT_HEIGHT      0x38u
#define ST_BUSY          1u
#define ST_DONE          2u
#define OP_COPY          1u
#define CACHE_FLUSH      0x2u
#define CACHE_INVAL      0x4u

#define SRC   0x05000000u          /* position-encoded source            */
#define DST   0x06000008u          /* dst+8 UNALIGNED, unique 0x0600 region */
#define W     24u
#define H     2u
#define STR   (W * 2u)             /* tight stride, bytes per row         */
#define SP    ((volatile uint16_t *)(unsigned long)SRC)
#define DP    ((volatile uint16_t *)(unsigned long)DST)

static inline uint16_t enc(unsigned x, unsigned y){ return (uint16_t)((x&0xFFu)|((y&0xFFu)<<8)); }

int main(void){
    unsigned pitch = STR/2u;
    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) SP[y*pitch+x]=enc(x,y);
    REG_CACHE_CTRL = CACHE_FLUSH;           /* push src to DDR once */

    printf("\n=== blit_capture: looping dst+8 COPY (SRC %08x -> DST %08x) ===\n", SRC, DST);
    unsigned pass=0;
    for (;;) {
        BR(BLIT_DST_ADDR)=DST; BR(BLIT_DST_STRIDE)=STR;
        BR(BLIT_SRC_ADDR)=SRC; BR(BLIT_SRC_STRIDE)=STR;
        BR(BLIT_WIDTH)=W;      BR(BLIT_HEIGHT)=H;
        BR(BLIT_CTRL)=1u|(OP_COPY<<1);
        while (BR(BLIT_STATUS)&ST_BUSY){}
        BR(BLIT_STATUS)=ST_DONE;
        REG_CACHE_CTRL = CACHE_INVAL;

        unsigned bad=0, fx=0, fy=0; uint16_t fg=0, fw=0;
        for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++){
            uint16_t got=DP[y*pitch+x], want=enc(x,y);
            if (got!=want){ if(!bad){fx=x;fy=y;fg=got;fw=want;} bad++; }
        }
        printf("pass %u: %s %u/%u wrong", pass, bad?"FAIL":"PASS", bad, W*H);
        if (bad) printf("  first(%u,%u) got=%04x want=%04x", fx, fy, fg, fw);
        printf("\n");
        pass++;
        for (volatile unsigned long d=0; d<3000000ul; d++){}   /* ~delay between blits */
    }
    return 0;
}
