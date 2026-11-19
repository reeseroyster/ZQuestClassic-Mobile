#ifndef ZC_SAVESTATE_H_
#define ZC_SAVESTATE_H_

#include <string>

// Emulator-style quick save states (one hidden state per quest).
// Unlike a .sav progress save, a state captures the LIVE world: exact hero
// position, every enemy/item/weapon/decoration/portal sprite, FFCs, the
// current screen's combo buffer and the moving block, on top of the full
// gamedata progress. Loading restores the exact moment, on the exact screen
// and dmap, even across maps.
//
// Both functions must run on the game thread (they touch render/script state).
bool savestate_quick_save(std::string& err);
bool savestate_quick_load(std::string& err);
// Per-quest state file path (files/quick/<quest>.state on Android).
std::string savestate_quick_path();
bool savestate_has_quick();

#endif
