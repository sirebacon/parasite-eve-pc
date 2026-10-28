#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pe_game.h"
#include "pe_mod_host.h"
#include "pe_mod_menu.h"
#include "pe_panacea.h"
#include "pe_services.h"
#include "pe1/game_state.h"
#include "pe1/item_ids.h"

SaveRuntimeState g_SaveRuntimeState;
GameStateBlock g_GameStateBlock;
int g_FieldMoveLock;
static Combatant aya;
static ItemDataRecord items[256];
static int loaded = 1, battle, economy = 1, service = 1, limit = 50;
static PeHookFn hooks[PE_HOOK_COUNT];
static PeMenuPerFrame ticks[3];
static int tick_count, *recipe;
static PeMenuChoiceCount recipe_count;
static PeMenuAction combine;
int pe_game_loaded(void) { return loaded; }
int pe_in_battle(void) { return battle; }
Combatant *pe_aya_core(void) { return &aya; }
ItemDataRecord *pe_base_item(unsigned id) { return id < 256 ? &items[id] : NULL; }
int Inv_GetAyaSlotLimit(void) { return limit; }
void pe_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void pe_toast(const char *text) { (void)text; }
static int economy_enabled(void) { return economy; }
static PeJunkApi junk = {.enabled = economy_enabled};
const void *pe_service_get(const char *name, int version) {
    assert(!strcmp(name,"junk") && version==1); return service ? &junk : NULL;
}
int pe_hook_add(int id, PeHookFn fn, void *u, int p, const char *m) {
    (void)u; (void)p; (void)m; hooks[id] = fn; return 0;
}
int pe_menu_register_tick(PeMenuPerFrame fn) { ticks[tick_count++] = fn; return 0; }
int pe_menu_register_dynamic_choice(const char *p, const char *l, const char *d, int *v,
                                    unsigned max, PeMenuChoiceCount c, PeMenuChoiceName n) {
    (void)p; (void)l; (void)d; (void)n; assert(max==4); recipe=v; recipe_count=c; return 0;
}
int pe_menu_register_info(const char *p, const char *l, const char *d, PeMenuText t) {
    (void)p; (void)l; (void)d; (void)t; return 0;
}
int pe_menu_register_action(const char *p, const char *l, const char *d, PeMenuAction a) {
    (void)p; (void)l; (void)d; combine=a; return 0;
}
static void bag(int a, int b) {
    memset(g_AyaInventoryItems,0,sizeof g_AyaSaveState.inventory_items);
    g_AyaInventoryItems[0]=a; g_AyaInventoryItems[1]=b; ticks[1]();
}
#include "pe1/menu_widget.h"
#include "pe1/menu_equipment.h"
#include "pe1/menu_state.h"
int g_MenuActiveItemList, g_MenuSelectionLocked, g_InvSelectedItemIndex=-1;
static int *combine_toggle;
static MenuWidgetNode root,panel,list;
void Menu_DrawItemList(MenuWidgetNode *n) { (void)n; }
void Menu_DrawSelectableEquipSlotList(MenuWidgetNode *n) { (void)n; }
int Menu_InventoryInputHandler(MenuWidgetNode *n,unsigned int f) { (void)n;(void)f;return 0; }
void Menu_ContextPick(int n) { (void)n; }
void Inv_SelectActiveList(int n) { (void)n; }
void MenuWidget_DrawCenteredText(u8 *t) { (void)t; }
void MenuWidget_DrawList(MenuWidgetNode *n,void (*f)(int)) { (void)n;(void)f; }
MenuWidgetNode *MenuWidget_GetChild(MenuWidgetNode *n,int i) { return n->children[i]; }
int MenuWidget_GridCellIndex(MenuWidgetNode *n) { return n->cursor_y; }
int Menu_StepEquipScreen(MenuWidgetNode *n,unsigned int f) { (void)n;(void)f;return 0; }
void Menu_PlayErrorSound(void) {}
void Menu_PlayConfirmSound(void) {}
void MenuWidget_DestroyNode(MenuWidgetNode *n) { (void)n; }
int pe_menu_register_toggle(const char *p,const char *l,const char *d,int *v,PeMenuPerFrame f) {
    (void)p;(void)l;(void)d;(void)f;combine_toggle=v;return 0;
}
static int colour(int slot) {
    int dimmed=1;
    PeHookInventoryRow row={&list,slot,&dimmed};hooks[PE_HOOK_INVENTORY_ROW](&row,NULL);
    return dimmed;
}
static void check_combine_colours(void) {
    root.draw=Menu_DrawItemList;panel.mode=1;panel.selected_base=1;
    panel.update=Menu_InventoryInputHandler;panel.parent=&root;
    list.draw=Menu_DrawSelectableEquipSlotList;list.parent=&panel;list.appearance.cursorTargetX=-1;
    loaded=service=economy=1;battle=0;limit=50;g_pe_panacea=1;*combine_toggle=1;
    g_AyaSaveState.current_hp=g_AyaSaveState.max_hp=999;
    bag(ITEM_MEDICINE_1,ITEM_MEDICINE_1);assert(!colour(0) && !colour(1));
    bag(ITEM_MEDICINE_2,ITEM_MEDICINE_2);assert(!colour(0));
    bag(ITEM_MEDICINE_3,ITEM_MEDICINE_3);assert(!colour(0));
    bag(ITEM_MEDICINE_4,ITEM_FULL_CURE);assert(!colour(0) && !colour(1));
    g_pe_panacea=0;assert(colour(0)==1 && colour(1)==1);g_pe_panacea=1;
    bag(ITEM_MEDICINE_1,0);assert(colour(0)==1 && colour(1)==1);
    bag(ITEM_MEDICINE_1,ITEM_MEDICINE_1);limit=1;assert(colour(0)==1);limit=50;
    *combine_toggle=0;assert(colour(0)==1);*combine_toggle=1;
    economy=0;assert(colour(0)==1);economy=1;
    service=0;assert(colour(0)==1);service=1;
    battle=1;assert(colour(0)==1);battle=0;
    loaded=0;assert(colour(0)==1);loaded=1;
    g_MenuSelectionLocked=1;assert(colour(0)==1);g_MenuSelectionLocked=0;
    g_InvSelectedItemIndex=0;assert(colour(0)==1);g_InvSelectedItemIndex=-1;
    list.appearance.cursorTargetX=0;assert(colour(0)==1);list.appearance.cursorTargetX=-1;
    panel.selected_base=0x33;assert(colour(0)==1);panel.selected_base=1;
    assert(colour(-1)==1 && colour(50)==1);
    puts("Combine row colour checks passed: full health, every recipe, missing partners, settings and menu/move/battle guards.");
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--panacea-off")) {
        pe_panacea_register("pe_rebalance");
        ItemDataRecord *r=&items[ITEM_ITEM25];
        r->flags=0x41;r->iconId=5;items[ITEM_MEDICINE_4].iconId=9;
        g_pe_panacea=0;ticks[0]();assert(r->flags==0x41 && r->iconId==5);
        u8 original[]={0xFF};void *text=original;
        PeHookTextLookup lookup={8,ITEM_ITEM25-1,&text};
        hooks[PE_HOOK_TEXT_LOOKUP](&lookup,NULL);assert(text==original);
        g_pe_panacea=1;ticks[0]();assert(r->flags==0x43 && r->iconId==9);
        g_pe_panacea=0;ticks[0]();assert(r->flags==0x41 && r->iconId==5);
        g_pe_panacea=1;ticks[0]();r->flags=0xA2;r->iconId=99;
        g_pe_panacea=0;ticks[0]();assert(r->flags==0xA2 && r->iconId==99);
        pe_panacea_shutdown();return 0;
    }
    pe_panacea_register("pe_rebalance"); pe_combine_register("pe_rebalance");
    assert(g_pe_panacea && recipe_count()==4);
    items[ITEM_ITEM25].flags=0x40; items[ITEM_ITEM25].iconId=5;
    items[ITEM_MEDICINE_4].iconId=9; ticks[0]();
    assert(items[ITEM_ITEM25].flags==0x43 && items[ITEM_ITEM25].iconId==9);
    void *text=NULL; PeHookTextLookup lookup={8,ITEM_ITEM25-1,&text};
    hooks[PE_HOOK_TEXT_LOOKUP](&lookup,NULL); assert(text && *(u8 *)text==0x1F);
    lookup.table=12; hooks[PE_HOOK_TEXT_LOOKUP](&lookup,NULL); assert(*(u8 *)text==0x21);
    lookup.index=0; text=NULL; hooks[PE_HOOK_TEXT_LOOKUP](&lookup,NULL); assert(!text);
    int item=ITEM_ITEM25;
    aya.stateFlags=AYA_POISON_MASK | 0x4 | 0x10 | 0x40 | 0x1000 | 0x100;
    g_FieldMoveLock=FIELD_MOVE_CONFUSED|0x80;
    hooks[PE_HOOK_ITEM_USE](&item,NULL);
    assert(item==ITEM_MEDICINE_4 && aya.stateFlags==(AYA_POISON_MASK|0x100) && g_FieldMoveLock==0x80);
    item=ITEM_MEDICINE_1; hooks[PE_HOOK_ITEM_USE](&item,NULL); assert(item==ITEM_MEDICINE_1);
    *recipe=0; bag(ITEM_MEDICINE_1,ITEM_MEDICINE_1); combine();
    assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1); combine();
    assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_2 && !g_AyaInventoryItems[1]);
    *recipe=1; bag(ITEM_MEDICINE_2,ITEM_MEDICINE_2); combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_3);
    *recipe=2; bag(ITEM_MEDICINE_3,ITEM_MEDICINE_3); combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_4);
    *recipe=3; bag(ITEM_FULL_CURE,ITEM_MEDICINE_4);
    for(int i=2;i<50;++i) g_AyaInventoryItems[i]=ITEM_MEDICINE_1;
    combine(); combine(); assert(!g_AyaInventoryItems[0] && g_AyaInventoryItems[1]==ITEM_ITEM25); /* full bag */
    *recipe=0; bag(ITEM_MEDICINE_1,0); combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1);
    bag(ITEM_MEDICINE_1,ITEM_MEDICINE_1); combine(); g_AyaInventoryItems[2]=ITEM_FULL_CURE;
    combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_2);
    bag(ITEM_MEDICINE_1,ITEM_MEDICINE_1); battle=1; combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1); battle=0;
    economy=0; combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1); economy=1;
    service=0; combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1); service=1;
    limit=1; combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1); limit=50;
    combine(); ticks[1](); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_1);
    g_pe_panacea=0; ticks[0](); assert(recipe_count()==3 && items[ITEM_ITEM25].flags==0x40);
    *recipe=3; bag(ITEM_MEDICINE_4,ITEM_FULL_CURE); combine(); combine(); assert(g_AyaInventoryItems[0]==ITEM_MEDICINE_4);
    item=ITEM_ITEM25; aya.stateFlags=1; hooks[PE_HOOK_ITEM_USE](&item,NULL); assert(item==ITEM_ITEM25 && aya.stateFlags==1);
    check_combine_colours();
    pe_panacea_shutdown(); assert(items[ITEM_ITEM25].flags==0x40 && items[ITEM_ITEM25].iconId==5);
    puts("Passed native Panacea/recipe checks: labels, flags/icon, cure/immunity preservation, all recipes, full bag, confirmation, changed bag, unavailable service/battle/disabled guards and cleanup.");
    return 0;
}
