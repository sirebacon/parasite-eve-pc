#include "common.h"
#include "pe1/boot_display.h"
#include "pe1/cdrom.h"
#include <psyz/cd.h>
#include <psyz/audio.h>
#include <string.h>

#include "playback_externs.h"

/* The original stream consumes the mounted image through PsyZ's XA decoder.
 * No drive queue, shell checks, retry loops or sector-address callbacks remain.
 * The audio lock protects playback state shared with the sample-pull thread. */
static PeDisc *playbackImage;
static int playbackEnded, playbackError;
s32 g_BootDisplayPlaybackActive;
s32 g_BootDisplayPlaybackEnded, g_BootDisplayReportedPos;

/* Caller holds audio: detaching waits for an in-flight sample read and prevents
 * any later one from touching the old image/buffer, even when CdStop fails. */
int BootDisplay_DetachPlayback(void) {
    int active=g_BootDisplayPlaybackActive;
    if(playbackImage)Psyz_CdSetReadCB(NULL);
    playbackImage=NULL;
    g_BootDisplayPlaybackActive=0;
    return active;
}

s32 BootDisplay_StartPlayback(void) {
    if(g_BootDisplayPlaybackActive)BootDisplay_WaitForCdAndRestoreCallbacks();
    if(!g_CdImage || !g_CdImage->stream || g_CdImage->sector_size!=2352 ||
       !g_BootDisplayPlaybackSectorBuffer ||
       g_BootDisplayPlaybackStartPos<0 ||
       g_BootDisplayPlaybackStartPos>=g_BootDisplayPlaybackEndPos)return -1;
    u8 mode=200, filter[2]={1,0};
    /* Initialize the audio mutex before publishing shared playback data.
     * CD commands run outside that lock: completion enters the IRQ kernel. */
    if(Psyz_AudioInit()<0)return -2;
    CdControlB(8,NULL,NULL);
    Psyz_AudioLock();
    playbackImage=g_CdImage;
    playbackEnded=playbackError=0;
    g_BootDisplayPlaybackEnded=g_BootDisplayPlaybackActive=0;
    Psyz_CdSetReadCB(BootDisplay_StepPlaybackTransfer);
    Psyz_AudioUnlock();
    if(CdControlB(14,&mode,NULL)<=0 || CdControlB(13,filter,NULL)<=0 ||
       BootDisplay_IssuePlaybackRead(200,27)<0){
        CdControlB(8,NULL,NULL);Psyz_AudioLock();Psyz_CdSetReadCB(NULL);
        playbackImage=NULL;Psyz_AudioUnlock();return -2;
    }
    Psyz_AudioLock();
    g_BootDisplayPlaybackActive=1;
    Psyz_AudioUnlock();
    VSyncCallback(BootDisplay_CheckPlaybackTime);
    return 0;
}

int BootDisplay_SetCdFilter(int code) {
    if((unsigned)code>255)return -1;
    u8 filter[2]={1,(u8)code};
    return CdControlB(13,filter,NULL)>0?0:-1;
}

int BootDisplay_IssuePlaybackRead(int command, int parameter) {
    if(command!=200 || parameter!=27 || !playbackImage)return -1;
    CdlLOC position;
    CdIntToPos(g_BootDisplayPlaybackStartPos,&position);
    if(CdControlB(2,(u8 *)&position,NULL)<=0)return -1;
    Psyz_AudioLock();
    g_BootDisplayPlaybackCurrentPos=g_BootDisplayPlaybackStartPos;
    Psyz_AudioUnlock();
    if(CdControlB(27,NULL,NULL)<=0)return -1;
    g_BootDisplayPlaybackReadCount=(s32)((u32)g_BootDisplayPlaybackReadCount+1);
    return 0;
}

s32 BootDisplay_StepPlaybackTransfer(struct PsyzCdRead *read) {
    if(!playbackImage)return -1;
    if(!read || read->size!=2352 || !read->buffer || !playbackImage ||
       playbackImage!=g_CdImage){playbackError=1;return -1;}
    if(read->sector>=(u32)g_BootDisplayPlaybackEndPos){playbackEnded=1;return -1;}
    if(read->sector<(u32)g_BootDisplayPlaybackStartPos ||
       !pe_disc_read_raw(playbackImage,&g_BootDisplayPlaybackFile,
          read->sector-(u32)g_BootDisplayPlaybackStartPos,read->buffer)){
        playbackError=1;return -1;
    }
    const u8 *sector=read->buffer;
    if((sector[0x12]&0x44)==0x44)++g_BootDisplayPlaybackAltCount;
    else{
        memcpy(g_BootDisplayPlaybackSectorBuffer+(g_BootDisplayPlaybackRingIndex<<11),sector+24,2048);
        g_BootDisplayPlaybackRingIndex=(g_BootDisplayPlaybackRingIndex+1)&7;
    }
    g_BootDisplayPlaybackCurrentPos=(s32)read->sector+1;
    return 2352;
}

void BootDisplay_CheckPlaybackTime(void) {
    if((VSync(-1)&31)!=0)return;
    int mute=0,query=0;
    Psyz_AudioLock();
    if(g_BootDisplayPlaybackActive){
        if(playbackError || playbackImage!=g_CdImage){g_BootDisplayState=g_BootDisplayDone=-1;g_BootDisplayPlaybackEnded=1;}
        else if(playbackEnded || g_BootDisplayPlaybackCurrentPos>=g_BootDisplayPlaybackEndPos){
            mute=1;g_BootDisplayPlaybackEnded=1;
        }else query=1;
    }
    Psyz_AudioUnlock();
    if(mute)Spu_SetVoiceVolume(0);
    if(query){
        u8 position[8]={0};
        int ok=CdControlB(16,NULL,position)>0;
        Psyz_AudioLock();
        if(g_BootDisplayPlaybackActive){
            if(ok)BootDisplay_RecordReadPosition(2,position);
            else{g_BootDisplayState=g_BootDisplayDone=-1;g_BootDisplayPlaybackEnded=1;}
        }
        Psyz_AudioUnlock();
    }
}

void BootDisplay_RecordReadPosition(u8 mode, const u8 *value) {
    if(mode==2 && value){
        CdlLOC position;memcpy(&position,value,sizeof(position));
        g_BootDisplayReportedPos=CdPosToInt(&position);
        if(g_BootDisplayReportedPos!=0)g_BootDisplayPlaybackCurrentPos=g_BootDisplayReportedPos;
    }
}
