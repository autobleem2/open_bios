/*

MIT License

Copyright (c) 2021 PCSX-Redux authors
Copyright (c) 2026 AutoBleem authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

// The shell's main() with the AutoBleem boot screen in place of the cube (main.c, kept as it is upstream). The
// state machine is upstream's: the intro, then idle until the CD-ROM code finds a disc; a good data disc plays
// the outro and returns to the kernel, which boots it; a bad one shows the error look until it changes.

#include "common/hardware/hwregs.h"
#include "common/hardware/pcsxhw.h"
#include "common/syscalls/syscalls.h"
#include "shell/ab_splash.h"
#include "shell/cdrom.h"
#include "shell/dcos.h"
#include "shell/gpu.h"
#include "shell/spu.h"

static void idle() {
    checkCD(0);
    checkSPU();
}

int main() {
    ramsyscall_printf("*** OpenBIOS tiny shell (AutoBleem) - starting ***\n");
    int wasLocked = enterCriticalSection();
    int isPAL = (*((char *)0xbfc7ff52) == 'E');
    unsigned fps = isPAL ? 50 : 60;
    generateCosTable();
    initGPU(isPAL);
    union Color black = {.r = 0, .g = 0, .b = 0};
    flip(0, black);
    abSplashInit(fps);
    initSPU();
    initCD();
    enableDisplay();
    int wasError = 0;
    int wasSuccess = 0;
    int boot = 0;
    while (1) {
        int ended = abSplashTick();
        enum AbState state = abSplashState();
        if (state == AB_STATE_INTRO && ended) {
            abSplashSetState(wasError ? AB_STATE_ERROR : boot ? AB_STATE_SUCCESS : AB_STATE_IDLE);
            if (boot) spuScheduleOutro();
        } else if (state == AB_STATE_SUCCESS && ended) {
            break;
        } else if (state == AB_STATE_IDLE && ended) {
            spuSchedulePing();  // the idle loop starts over with its ping
        }
        checkCD(fps);
        int isError = isCDError();
        int isSuccess = isCDSuccess();
        if (isError && !wasError) {
            wasError = 1;
            spuScheduleError();
            if (abSplashState() != AB_STATE_INTRO) abSplashSetState(AB_STATE_ERROR);
            pcsx_message("Invalid disc inserted, or no disc.\nUse File->Open ISO to insert a valid PlayStation disc.");
        } else if (isSuccess && !wasSuccess) {
            wasSuccess = 1;
            if (!isCDAudio()) {
                ramsyscall_printf("*** Data is acceptable, booting now. ***\n");
                boot = 1;
                if (abSplashState() != AB_STATE_INTRO) {
                    abSplashSetState(AB_STATE_SUCCESS);
                    spuScheduleOutro();
                }
            } else {
                // todo: play audio cd
            }
        } else if (!isError && wasError) {
            wasError = 0;
            spuScheduleIdle();
            if (abSplashState() != AB_STATE_INTRO) abSplashSetState(AB_STATE_IDLE);
        }
        waitVSync(1, idle);
        flip(0, abSplashBackground());
        abSplashRender();
    }
    uninitSPU();
    if (!wasLocked) leaveCriticalSection();
}
