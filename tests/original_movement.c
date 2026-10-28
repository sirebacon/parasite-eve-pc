#include "pe1/entity_frame_update.h"
#include "pe1/entity_allocation.h"
#include "pe1/script_state.h"
#include "pe1/script_queue.h"
#include "pe1/scene_flags.h"
#include <stdio.h>
#ifdef PE_MODS
#include "../mods/host/pe_mod_host.h"
static int entry_factor;
static int entry_speed_hook(void *ctx, void *unused) {
    PeHookActorStep *step=ctx;(void)unused;
    if(step->speed)*step->speed=(int)((int64_t)*step->speed*entry_factor/65536);
    return 0;
}
#endif
/* Controlled input/dispatch tables are fixtures; game code is unchanged. */
SceneFlagRecord *g_EntityInputRulesByMode[18];
static unsigned rule_calls;
static void input(struct BattleEntity *ctx,int *mode)
{
    FieldActor *actor=(FieldActor *)ctx;(void)mode;
    ++rule_calls;actor->accel_x=actor->pad_0AC[5];
}
int main(void)
{
    FieldActor actor={0};FieldActorState state={0};
    SceneFlagRecord rules[]={{0x20,0,0,input},{0,0,0,0}};
    u32 script[]={10|(2u<<13)|(1u<<17),0,5,3,2|(1u<<13),0,1};
    ScriptNode *node;
    Script_InitNodePool();Script_InitNodeFreeList();
    node=Script_AllocNode(script,0);actor.script_node_lists[0]=node;
    actor.state=&state;actor.pos_x=100;actor.rot_y=7;
    actor.flags=2;actor.gravity_x=2;actor.motion_x=4;actor.delta_x=5;
    g_PlayerEntity=&actor;g_EntityInputRulesByMode[0]=rules;
    g_ScriptQueueOpcodeHandlers[10]=Script_AssignValue;
    g_ScriptQueueOpcodeHandlers[2]=Entity_SetCurrentNodeActive;
    g_FieldActorTypeCallbacks[0].frame(&actor);
    if (actor.base_x!=100 || actor.saved_rot_y!=7 || actor.motion_x!=9 ||
        actor.pos_x!=114 || actor.pad_0AC[5]!=3 || rule_calls!=1) return 1;
    actor.base_x=999;g_GameStateFlags=0x100;
    node->current.script=script;node->active=1;actor.pad_0AC[5]=0;
    g_FieldActorTypeCallbacks[12].frame(&actor);
    if (actor.pos_x!=114 || actor.base_x!=999 || actor.pad_0AC[5]!=3) return 2;
    g_GameStateFlags=0;actor.script_node_lists[0]=0;
    actor.flags=0;actor.accel_x=0;actor.motion_x=1;actor.delta_x=0;actor.pos_x=INT32_MAX;
    Entity_IntegratePositionConditional(&actor);
    if (actor.pos_x!=INT32_MIN || actor.base_x!=INT32_MAX) return 3;
    g_FieldMoveLock=1;
    Entity_IntegratePositionFull(&actor);
    if (rule_calls!=1) return 4;
    g_FieldMoveLock=0;state.flags=0x80;actor.mode=1;
    g_EntityInputRulesByMode[17]=rules;
    Entity_IntegratePositionFull(&actor);
    if (rule_calls!=2) return 5;
    {
        u32 second[]={10|(2u<<13)|(1u<<17),0,5,20,2|(1u<<13),0,1};
        u32 third[]={10|(2u<<13)|(1u<<17),0,5,30,2|(1u<<13),0,1};
        actor.script_node_lists[0]=node;node->current.script=script;node->active=1;
        actor.script_node_lists[1]=Script_AllocNode(second,0);
        actor.script_node_lists[2]=Script_AllocNode(third,0);
        state.flags=0;actor.mode=0;actor.flags=0;actor.pos_x=0;
        actor.motion_x=actor.accel_x=actor.delta_x=0;
        Entity_IntegratePositionFull(&actor);
        if (actor.pad_0AC[5]!=30 || actor.accel_x!=30 || actor.pos_x!=30) return 6;
    }
#ifdef PE_MODS
    PeHookInit init={0,"/tmp/pe-movement-clamp-test/parasite-eve.toml"};
    pe_host_init(&init);
    if(pe_hook_add(PE_HOOK_ACTOR_STEP,entry_speed_hook,NULL,0,"host"))return 7;
    const int factors[]={65536,98304,131072,196608,262144};
    unsigned arrivals=0;
    int (*const moves[])(int **)={Script_MoveTowardPoint,Render_CameraTrackEntityAngle,
        Render_CameraTrackRelativeOffset,Render_CameraTrackEntityZoom};
    for(unsigned method=0;method<4;++method)
    for(unsigned choice=0;choice<5;++choice)for(unsigned axis=0;axis<4;++axis)
    for(int distance=1;distance<=60;++distance) {
        actor=(FieldActor){0};actor.state=&state;actor.move_factor=65536;actor.move_speed=4096;
        g_PlayerEntity=g_CurrentEntity=&actor;
        ScriptNode move_node={0};g_ScriptNodePool=&move_node;
        int x=axis==0?distance*65536:axis==1?-distance*65536:0;
        int z=axis==2?distance*65536:axis==3?-distance*65536:0;
        int turn=0,type=6,sub=0;int *args[]={&x,&z,&turn};entry_factor=factors[choice];
        FieldActor target={0};target.type_id=type;target.pos_x=x;target.pos_z=z;
        g_FieldActorListHead=&target;
        if(method==3){args[0]=&type;args[1]=&sub;}
        int arrived=0;
        for(unsigned frame=0;frame<20;++frame) {
            g_ScriptCursorAddress=(uintptr_t)(script+5);
            if(moves[method](args)) {arrived=1;break;}
            actor.pos_x+=actor.motion_x;actor.pos_z+=actor.motion_z;
        }
        if(!arrived || actor.pos_x!=x || actor.pos_z!=z || actor.motion_x || actor.motion_z ||
           actor.move_factor!=65536 || actor.anim_step)return 8;
        ++arrivals;
    }
    printf("Original scripted movement: %u exact destination arrivals at all five entry speeds passed\n",arrivals);
#endif
    g_PlayerEntity=g_FieldActorListHead=0;g_FieldMoveLock=0;
    puts("Original callback-before-motion order, freeze, input locks and word wrap passed");
    return 0;
}
