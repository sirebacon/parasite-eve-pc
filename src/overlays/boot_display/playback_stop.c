#include "common.h"
#include "pe1/boot.h"
#include "pe1/cdrom.h"
#include "pe1/psyq_callbacks.h"
#include "pe1/psyq_cd.h"
#include "pe1/boot_display.h"
#include <psyz/cd.h>
#include <psyz/audio.h>

s32 BootDisplay_WaitForCdAndRestoreCallbacks(void) {
    s32 result=0;
    Psyz_AudioLock();
    int active=BootDisplay_DetachPlayback();
    Psyz_AudioUnlock();
    if(active){
        /* CD completion enters the kernel; IRQ callbacks may need audio. */
        if(CdControlB(8,NULL,NULL)<=0)result=-1;
    }
    VSyncCallback(Boot_VsyncCallback);
    return result;
}
