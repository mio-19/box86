#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <stddef.h>

#include "debug.h"
#include "box86stack.h"
#include "x86emu.h"
#include "x86run.h"
#include "x86emu_private.h"
#include "x86run_private.h"
#include "x86primop.h"
#include "x86trace.h"
#include "x87emu_private.h"
#include "box86context.h"
#include "bridge.h"
#include "signals.h"

void print_banner(x86emu_t* ref)
{
    printf_log(LOG_NONE, "Warning, difference between Interpreter and Dynarec in %p (%02x %02x %02x %02x %02x %02x %02x %02x)\n"\
        "=======================================\n", 
        (void*)ref->old_ip, 
        ((uint8_t*)ref->old_ip)[0], ((uint8_t*)ref->old_ip)[1], ((uint8_t*)ref->old_ip)[2], ((uint8_t*)ref->old_ip)[3],
        ((uint8_t*)ref->old_ip)[4], ((uint8_t*)ref->old_ip)[5], ((uint8_t*)ref->old_ip)[6], ((uint8_t*)ref->old_ip)[7]
    );
    printf_log(LOG_NONE, "DIFF: Dynarec |  Interpreter\n----------------------\n");
}
#define BANNER if(!banner) {banner=1; print_banner(ref);}
void x86test_check(x86emu_t* ref, uintptr_t ip)
{
    int banner = 0;
    x86test_t* test = &ref->test;
    x86emu_t* emu = test->emu;
    if(memcmp(ref->regs, emu->regs, sizeof(emu->regs))) {
        static const char* regname[] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI"};
        BANNER;
        for(int i=0; i<8; ++i) {
            if(ref->regs[i].dword[0]!=emu->regs[i].dword[0]) {
                printf_log(LOG_NONE, "%s: %08x | %08x\n", regname[i], ref->regs[i].dword[0], emu->regs[i].dword[0]);
            }
        }
    }
    if(ip!=emu->ip.dword[0]) {
        BANNER;
        printf_log(LOG_NONE, "RIP: %08x | %08x\n", ip, emu->ip.dword[0]);
    }
    // flags are volatile, so don't test them
	//memcpy(&ref->eflags, &emu->eflags, sizeof(emu->eflags));
    if(memcmp(ref->segs, emu->segs, sizeof(emu->segs))) {
        static const char* segname[] = {"CS", "DS", "ES", "SS", "FS", "GS"};
        BANNER;
        for(int i=0; i<6; ++i) {
            if(ref->segs[i]!=emu->segs[i]) {
                printf_log(LOG_NONE, "%s: %04x | %04x\n", segname[i], ref->segs[i], emu->segs[i]);
            }
        }
    }
    if(ref->top != emu->top) {
        BANNER;
        printf_log(LOG_NONE, "X87 TOP: %d | %d\n", ref->top, emu->top);
    }
    if(ref->fpu_stack != emu->fpu_stack) {
        BANNER;
        printf_log(LOG_NONE, "X87 STACK: %d | %d\n", ref->fpu_stack, emu->fpu_stack);
    }
	if(ref->fpu_stack && memcmp(ref->x87, emu->x87, sizeof(emu->x87))) {
        // need to check each regs, unused one might have different left over value
        for(int i=0; i<ref->fpu_stack; ++i) {
            if(ref->x87[(ref->top+i)&7].d != emu->x87[(emu->top+i)&7].d) {
                BANNER;
                printf_log(LOG_NONE, "ST%d: %g (%a) | %g (%a)\n", i, ref->x87[(ref->top+i)&7].d, ref->x87[(ref->top+i)&7].d, emu->x87[(emu->top+i)&7].d, emu->x87[(emu->top+i)&7].d);
            }
        }
    }
    //memcpy(ref->fpu_ld, emu->fpu_ld, sizeof(emu->fpu_ld));
    //memcpy(ref->fpu_ll, emu->fpu_ll, sizeof(emu->fpu_ll));
	/*if(ref->p_regs != emu->p_regs) {
        BANNER;
        printf_log(LOG_NONE, "X87 PREG: %x | %x\n", ref->p_regs, emu->p_regs);
    }*/
	if(ref->cw.x16 != emu->cw.x16) {
        BANNER;
        printf_log(LOG_NONE, "X87 CW: %x | %x\n", ref->cw.x16, emu->cw.x16);
    }
	if(ref->sw.x16 != emu->sw.x16) {
        BANNER;
        printf_log(LOG_NONE, "X87 SW: %x | %x\n", ref->sw.x16, emu->sw.x16);
    }
	if(memcmp(ref->mmx, emu->mmx, sizeof(emu->mmx))) {
        BANNER;
        for(int i=0; i<8; ++i) {
            if(ref->mmx[i].q!=emu->mmx[i].q) {
                printf_log(LOG_NONE, "EMM[%d]: %016llx | %016llx\n", i, ref->mmx[i].q, emu->mmx[i].q);
            }
        }
    }
    if(ref->mxcsr.x32 != emu->mxcsr.x32) {
        BANNER;
        printf_log(LOG_NONE, "MXCSR: %x | %x\n", ref->mxcsr.x32, emu->mxcsr.x32);
    }
    if(memcmp(ref->xmm, emu->xmm, sizeof(emu->xmm))) {
        BANNER;
        for(int i=0; i<8; ++i) {
            if(ref->xmm[i].q[0]!=emu->xmm[i].q[0] || ref->xmm[i].q[1]!=emu->xmm[i].q[1] ) {
                printf_log(LOG_NONE, "XMM[%02d]: %016llx-%016llx | %016llx-%016llx\n", i, ref->xmm[i].q[1], ref->xmm[i].q[0], emu->xmm[i].q[1], emu->xmm[i].q[0]);
            }
        }
    }
    if(test->memsize) {
        if(memcmp(test->mem, (void*)test->memaddr, test->memsize)) {
            BANNER;
            printf_log(LOG_NONE, "MEM: @%p :", (void*)test->memaddr);
            for(int i=0; i<test->memsize; ++i)
                printf_log(LOG_NONE, " %02x", test->mem[i]);
            printf_log(LOG_NONE, " |");
            for(int i=0; i<test->memsize; ++i)
                printf_log(LOG_NONE, " %02x", ((uint8_t*)test->memaddr)[i]);
            printf_log(LOG_NONE, "\n");
        }
    }
    if(banner)  // there was an error, re-sync!
        CopyEmu(emu, ref);
}
#undef BANNER

void x86test_init(x86emu_t* ref, uintptr_t ip)
{
    x86test_t* test = &ref->test;
    if(!test->test) {
        test->clean = 0;
        return;
    }
    // check if test as a valid emu struct
    if(!test->emu) {
        test->emu = NewX86Emu(my_context, ip, (uintptr_t)ref->init_stack, ref->size_stack, 0);
        CopyEmu(test->emu, ref);
    } else {
        // check if IP is same, else, sync
        uintptr_t prev_ip = test->emu->ip.dword[0];
        if(test->clean)
            x86test_check(ref, ip);
        if(ip != prev_ip || !test->test || !test->clean) {
            CopyEmu(test->emu, ref);
        }
    }
    // Do a Dry single Step
    test->memsize = 0;
    test->clean = 1;
    ref->old_ip = ip;
    RunTest(test);
    // this will be anakyzed next step
}
