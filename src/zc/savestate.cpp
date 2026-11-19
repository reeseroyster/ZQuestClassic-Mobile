// Emulator-style quick save states for the Android port.
//
// A quick state is a single per-quest binary file (files/quick/<quest>.state)
// that embeds:
//   1. the full gamedata progress, serialized with the engine's own save
//      writer (_write_save) so named-slot saves and quick states agree, and
//   2. a snapshot of the LIVE world: screen combo buffers + screen state,
//      FFCs, every sprite list (guys/items/Ewpns/Lwpns/chainlinks/decorations/
//      portals) with per-subclass fields, the moving block, Hero, and the
//      moment globals (freeze flags, frame counters, ...).
//
// Loading re-enters the saved screen through the engine's own cont_game()
// path (which handles cross-dmap travel, palettes, dmap scripts), then
// overwrites the freshly loaded screen/sprites with the snapshot. Sprites are
// reconstructed with their real constructors and their saved UIDs, so
// parent/child and summoner links are relinked exactly. Script *engine* state
// (running ZScript instances) is not copied; sprite/FFC/screen scripts are
// deterministically re-initialized from the saved script configs on the next
// frame, which is the same baseline a screen load produces.
//
// Must run on the game thread (the main loop polls the JNI request flags).

#include "zc/savestate.h"
#include "zc/saves.h"
#include "zc/zelda.h"
#include "zc/maps.h"
#include "zc/hero.h"
#include "zc/guys.h"
#include "zc/weapons.h"
#include "zc/decorations.h"
#include "zc/ffscript.h"
#include "zc/combos.h"
#include "items.h"
#include "ffc.h"
#include "iter.h"
#include "subscr.h"
#include "zconfig.h"
#include "core/mapscr.h"
#include "base/general.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <filesystem>
#include <type_traits>

namespace fs = std::filesystem;

namespace
{

// ---------------------------------------------------------------------------
// Per-quest path (mirrors saves.cpp's quick folder: beside saves/, hidden
// from the named-slot list).
// ---------------------------------------------------------------------------
fs::path save_folder_path()
{
	std::string path = zc_get_config("zeldadx", "save_folder", "saves");
	if(auto pos = path.find(" #?"); pos != std::string::npos)
		path = path.substr(0, pos);
	return path;
}

std::string quest_key()
{
	std::string base = (game && !game->header.qstpath.empty())
		? game->header.qstpath : "default";
	std::string leaf = fs::path(base).filename().string();
	if(leaf.size() > 4 && leaf.substr(leaf.size()-4) == ".qst")
		leaf.resize(leaf.size()-4);
	return leaf;
}

fs::path state_path()
{
	return save_folder_path().parent_path() / "quick" / (quest_key() + ".state");
}

// ---------------------------------------------------------------------------
// Binary IO. Reader carries an ok flag so a truncated/corrupt file aborts the
// whole load BEFORE any live state is touched.
// ---------------------------------------------------------------------------
struct BW
{
	FILE* f;
	bool ok = true;
	void raw(void const* p, size_t n) { if(ok && fwrite(p,1,n,f)!=n) ok=false; }
	template<class T> void pod(T const& v)
	{
		static_assert(std::is_trivially_copyable_v<T>, "pod only");
		raw(&v, sizeof v);
	}
	void zf(zfix const& v) { pod(int32_t(v.val)); }
	void str(std::string const& s) { pod(uint32_t(s.size())); raw(s.data(), s.size()); }
	void scr(script_config const& c)
	{
		pod(c.script);
		for(int a : c.run_args) pod(a);
		pod(uint32_t(c.inst_init.size()));
		for(auto const& kv : c.inst_init) { pod(kv.first); pod(kv.second); }
	}
};

struct BR
{
	FILE* f;
	bool ok = true;
	bool raw(void* p, size_t n)
	{
		if(!ok) return false;
		if(fread(p,1,n,f)!=n) ok=false;
		return ok;
	}
	template<class T> bool pod(T& v)
	{
		static_assert(std::is_trivially_copyable_v<T>, "pod only");
		return raw(&v, sizeof v);
	}
	bool zf(zfix& v) { int32_t r; if(!pod(r)) return false; v.val = r; return true; }
	bool str(std::string& s)
	{
		uint32_t n; if(!pod(n)) return false;
		if(n > 16u*1024*1024) { ok=false; return false; }
		s.resize(n);
		return raw(s.data(), n);
	}
	bool scr(script_config& c)
	{
		if(!pod(c.script)) return false;
		for(int& a : c.run_args) if(!pod(a)) return false;
		uint32_t n; if(!pod(n)) return false;
		if(n > 4096) { ok=false; return false; }
		c.inst_init.clear();
		for(uint32_t i=0; i<n; ++i)
		{
			word k; int v;
			if(!pod(k) || !pod(v)) return false;
			c.inst_init[k] = v;
		}
		return true;
	}
};

// ---------------------------------------------------------------------------
// Sprite base (solid_object + sprite POD fields). Pointers are NOT stored;
// parent is kept as a UID and relinked after every sprite exists.
// ---------------------------------------------------------------------------
struct SpriteRec
{
	int32_t uid = 0, parent_uid = 0;
	bool solid = false, temp_nonsolid = false;
	zfix x,y,vx,vy,old_x,old_y,old_x2,old_y2;
	int32_t hit_width=16,hit_height=16,hxofs=0,hyofs=0,sxofs=0,syofs=0,sxsz_ofs=0,sysz_ofs=0,solidflags=0,dir=-1;
	bool switch_hooked=false;
	uint8_t screen_spawned=0,current_screen=0;
	zfix z,fall,fakefall,fakez;
	int32_t tile=0,shadowtile=0,cs=0,flip=0,c_clk=0,clk=0,misc=0;
	int16_t flickercolor=0,flickertransp=0;
	zfix xofs,yofs,zofs,shadowxofs,shadowyofs;
	int32_t hzsz=0,txsz=-1,tysz=-1,id=0;
	zfix slopeid;
	byte onplatid=0;
	bool angular=false,can_freeze_from_holdup=false,is_frozen=false,is_within_freeze_viewport=true,isolated_freeze_viewport=false;
	double angle=0;
	int32_t lasthit=0,lasthitclk=0;
	int32_t dummy_int[10]{};
	zfix dummy_fix[10];
	float dummy_float[10]{};
	bool dummy_bool[10]{};
	int32_t drawstyle=0,extend=0;
	int32_t miscellaneous[32]{};
	int32_t script_no_colldet=0,wpnsprite=0;
	dword scriptflag=0;
	script_config scrconfig;
	int32_t scripttile=0;
	signed char scriptflip=0;
	bool do_animation=false;
	int32_t rotation=0,scale=0;
	uint32_t moveflags=0;
	byte drawflags=0,knockbackflags=0,screenedge=0;
	int32_t scriptshadowtile=0;
	byte knockbackSpeed=0;
	int32_t script_knockback_clk=0,script_knockback_speed=0,pit_pulldir=0,pit_pullclk=0,fallclk=0,fallCombo=0,o_tile=0,o_cset=0,drownclk=0,drownCombo=0;
	bool isspawning=false,can_flicker=false,hide_hitbox=false,behind=false;
	int32_t invis_timer=0;
	word spr_shadow=0,spr_death=0,spr_spawn=0;
	int16_t spr_death_anim_clk=0,spr_spawn_anim_clk=0;
	byte spr_death_anim_frm=0,spr_spawn_anim_frm=0;
	byte glowRad=0,glowShape=0;
	int32_t glowOffset=0,ignore_delete=0;
	zfix custom_gravity,custom_terminal_v;
	int32_t viewport_suspend_range=0,viewport_despawn_range=0;
};

void capture_sprite(sprite const& s, SpriteRec& r)
{
	r.uid = s.uid;
	r.parent_uid = s.parent ? s.parent->uid : 0;
	r.solid = s.getSolid();
	r.temp_nonsolid = s.getTempNonsolid();
	r.x=s.x; r.y=s.y; r.vx=s.vx; r.vy=s.vy;
	r.old_x=s.old_x; r.old_y=s.old_y; r.old_x2=s.old_x2; r.old_y2=s.old_y2;
	r.hit_width=s.hit_width; r.hit_height=s.hit_height;
	r.hxofs=s.hxofs; r.hyofs=s.hyofs; r.sxofs=s.sxofs; r.syofs=s.syofs;
	r.sxsz_ofs=s.sxsz_ofs; r.sysz_ofs=s.sysz_ofs; r.solidflags=s.solidflags; r.dir=s.dir;
	r.switch_hooked=s.switch_hooked;
	r.screen_spawned=s.screen_spawned; r.current_screen=s.current_screen;
	r.z=s.z; r.fall=s.fall; r.fakefall=s.fakefall; r.fakez=s.fakez;
	r.tile=s.tile; r.shadowtile=s.shadowtile; r.cs=s.cs; r.flip=s.flip;
	r.c_clk=s.c_clk; r.clk=s.clk; r.misc=s.misc;
	r.flickercolor=s.flickercolor; r.flickertransp=s.flickertransp;
	r.xofs=s.xofs; r.yofs=s.yofs; r.zofs=s.zofs;
	r.shadowxofs=s.shadowxofs; r.shadowyofs=s.shadowyofs;
	r.hzsz=s.hzsz; r.txsz=s.txsz; r.tysz=s.tysz; r.id=s.id;
	r.slopeid=s.slopeid; r.onplatid=s.onplatid;
	r.angular=s.angular; r.can_freeze_from_holdup=s.can_freeze_from_holdup;
	r.is_frozen=s.is_frozen; r.is_within_freeze_viewport=s.is_within_freeze_viewport;
	r.isolated_freeze_viewport=s.isolated_freeze_viewport;
	r.angle=s.angle;
	r.lasthit=s.lasthit; r.lasthitclk=s.lasthitclk;
	for(int i=0;i<10;i++)
	{
		r.dummy_int[i]=s.dummy_int[i]; r.dummy_fix[i]=s.dummy_fix[i];
		r.dummy_float[i]=s.dummy_float[i]; r.dummy_bool[i]=s.dummy_bool[i];
	}
	r.drawstyle=s.drawstyle; r.extend=s.extend;
	for(int i=0;i<32;i++) r.miscellaneous[i]=s.miscellaneous[i];
	r.script_no_colldet=s.script_no_colldet; r.wpnsprite=s.wpnsprite;
	r.scriptflag=s.scriptflag; r.scrconfig=s.scrconfig;
	r.scripttile=s.scripttile; r.scriptflip=s.scriptflip;
	r.do_animation=s.do_animation; r.rotation=s.rotation; r.scale=s.scale;
	r.moveflags=s.moveflags;
	r.drawflags=s.drawflags; r.knockbackflags=s.knockbackflags; r.screenedge=s.screenedge;
	r.scriptshadowtile=s.scriptshadowtile; r.knockbackSpeed=s.knockbackSpeed;
	r.script_knockback_clk=s.script_knockback_clk; r.script_knockback_speed=s.script_knockback_speed;
	r.pit_pulldir=s.pit_pulldir; r.pit_pullclk=s.pit_pullclk;
	r.fallclk=s.fallclk; r.fallCombo=s.fallCombo;
	r.o_tile=s.o_tile; r.o_cset=s.o_cset;
	r.drownclk=s.drownclk; r.drownCombo=s.drownCombo;
	r.isspawning=s.isspawning; r.can_flicker=s.can_flicker;
	r.hide_hitbox=s.hide_hitbox; r.behind=s.behind;
	r.invis_timer=s.invis_timer;
	r.spr_shadow=s.spr_shadow; r.spr_death=s.spr_death; r.spr_spawn=s.spr_spawn;
	r.spr_death_anim_clk=s.spr_death_anim_clk; r.spr_spawn_anim_clk=s.spr_spawn_anim_clk;
	r.spr_death_anim_frm=s.spr_death_anim_frm; r.spr_spawn_anim_frm=s.spr_spawn_anim_frm;
	r.glowRad=s.glowRad; r.glowShape=s.glowShape; r.glowOffset=s.glowOffset;
	r.ignore_delete=s.ignore_delete;
	r.custom_gravity=s.custom_gravity; r.custom_terminal_v=s.custom_terminal_v;
	r.viewport_suspend_range=s.viewport_suspend_range;
	r.viewport_despawn_range=s.viewport_despawn_range;
}

void write_sprite(BW& w, SpriteRec const& r)
{
	w.pod(r.uid); w.pod(r.parent_uid); w.pod(r.solid); w.pod(r.temp_nonsolid);
	w.zf(r.x); w.zf(r.y); w.zf(r.vx); w.zf(r.vy);
	w.zf(r.old_x); w.zf(r.old_y); w.zf(r.old_x2); w.zf(r.old_y2);
	w.pod(r.hit_width); w.pod(r.hit_height); w.pod(r.hxofs); w.pod(r.hyofs);
	w.pod(r.sxofs); w.pod(r.syofs); w.pod(r.sxsz_ofs); w.pod(r.sysz_ofs);
	w.pod(r.solidflags); w.pod(r.dir); w.pod(r.switch_hooked);
	w.pod(r.screen_spawned); w.pod(r.current_screen);
	w.zf(r.z); w.zf(r.fall); w.zf(r.fakefall); w.zf(r.fakez);
	w.pod(r.tile); w.pod(r.shadowtile); w.pod(r.cs); w.pod(r.flip);
	w.pod(r.c_clk); w.pod(r.clk); w.pod(r.misc);
	w.pod(r.flickercolor); w.pod(r.flickertransp);
	w.zf(r.xofs); w.zf(r.yofs); w.zf(r.zofs); w.zf(r.shadowxofs); w.zf(r.shadowyofs);
	w.pod(r.hzsz); w.pod(r.txsz); w.pod(r.tysz); w.pod(r.id);
	w.zf(r.slopeid); w.pod(r.onplatid);
	w.pod(r.angular); w.pod(r.can_freeze_from_holdup); w.pod(r.is_frozen);
	w.pod(r.is_within_freeze_viewport); w.pod(r.isolated_freeze_viewport);
	w.pod(r.angle);
	w.pod(r.lasthit); w.pod(r.lasthitclk);
	for(int i=0;i<10;i++)
	{
		w.pod(r.dummy_int[i]); w.zf(r.dummy_fix[i]);
		w.pod(r.dummy_float[i]); w.pod(r.dummy_bool[i]);
	}
	w.pod(r.drawstyle); w.pod(r.extend);
	for(int i=0;i<32;i++) w.pod(r.miscellaneous[i]);
	w.pod(r.script_no_colldet); w.pod(r.wpnsprite);
	w.pod(r.scriptflag); w.scr(r.scrconfig);
	w.pod(r.scripttile); w.pod(r.scriptflip);
	w.pod(r.do_animation); w.pod(r.rotation); w.pod(r.scale);
	w.pod(r.moveflags);
	w.pod(r.drawflags); w.pod(r.knockbackflags); w.pod(r.screenedge);
	w.pod(r.scriptshadowtile); w.pod(r.knockbackSpeed);
	w.pod(r.script_knockback_clk); w.pod(r.script_knockback_speed);
	w.pod(r.pit_pulldir); w.pod(r.pit_pullclk);
	w.pod(r.fallclk); w.pod(r.fallCombo); w.pod(r.o_tile); w.pod(r.o_cset);
	w.pod(r.drownclk); w.pod(r.drownCombo);
	w.pod(r.isspawning); w.pod(r.can_flicker); w.pod(r.hide_hitbox); w.pod(r.behind);
	w.pod(r.invis_timer);
	w.pod(r.spr_shadow); w.pod(r.spr_death); w.pod(r.spr_spawn);
	w.pod(r.spr_death_anim_clk); w.pod(r.spr_spawn_anim_clk);
	w.pod(r.spr_death_anim_frm); w.pod(r.spr_spawn_anim_frm);
	w.pod(r.glowRad); w.pod(r.glowShape); w.pod(r.glowOffset);
	w.pod(r.ignore_delete);
	w.zf(r.custom_gravity); w.zf(r.custom_terminal_v);
	w.pod(r.viewport_suspend_range); w.pod(r.viewport_despawn_range);
}

bool read_sprite(BR& b, SpriteRec& r)
{
	if(!b.pod(r.uid) || !b.pod(r.parent_uid) || !b.pod(r.solid) || !b.pod(r.temp_nonsolid)) return false;
	if(!b.zf(r.x) || !b.zf(r.y) || !b.zf(r.vx) || !b.zf(r.vy)) return false;
	if(!b.zf(r.old_x) || !b.zf(r.old_y) || !b.zf(r.old_x2) || !b.zf(r.old_y2)) return false;
	if(!b.pod(r.hit_width) || !b.pod(r.hit_height) || !b.pod(r.hxofs) || !b.pod(r.hyofs)) return false;
	if(!b.pod(r.sxofs) || !b.pod(r.syofs) || !b.pod(r.sxsz_ofs) || !b.pod(r.sysz_ofs)) return false;
	if(!b.pod(r.solidflags) || !b.pod(r.dir) || !b.pod(r.switch_hooked)) return false;
	if(!b.pod(r.screen_spawned) || !b.pod(r.current_screen)) return false;
	if(!b.zf(r.z) || !b.zf(r.fall) || !b.zf(r.fakefall) || !b.zf(r.fakez)) return false;
	if(!b.pod(r.tile) || !b.pod(r.shadowtile) || !b.pod(r.cs) || !b.pod(r.flip)) return false;
	if(!b.pod(r.c_clk) || !b.pod(r.clk) || !b.pod(r.misc)) return false;
	if(!b.pod(r.flickercolor) || !b.pod(r.flickertransp)) return false;
	if(!b.zf(r.xofs) || !b.zf(r.yofs) || !b.zf(r.zofs) || !b.zf(r.shadowxofs) || !b.zf(r.shadowyofs)) return false;
	if(!b.pod(r.hzsz) || !b.pod(r.txsz) || !b.pod(r.tysz) || !b.pod(r.id)) return false;
	if(!b.zf(r.slopeid) || !b.pod(r.onplatid)) return false;
	if(!b.pod(r.angular) || !b.pod(r.can_freeze_from_holdup) || !b.pod(r.is_frozen)) return false;
	if(!b.pod(r.is_within_freeze_viewport) || !b.pod(r.isolated_freeze_viewport)) return false;
	if(!b.pod(r.angle)) return false;
	if(!b.pod(r.lasthit) || !b.pod(r.lasthitclk)) return false;
	for(int i=0;i<10;i++)
	{
		if(!b.pod(r.dummy_int[i]) || !b.zf(r.dummy_fix[i])) return false;
		if(!b.pod(r.dummy_float[i]) || !b.pod(r.dummy_bool[i])) return false;
	}
	if(!b.pod(r.drawstyle) || !b.pod(r.extend)) return false;
	for(int i=0;i<32;i++) if(!b.pod(r.miscellaneous[i])) return false;
	if(!b.pod(r.script_no_colldet) || !b.pod(r.wpnsprite)) return false;
	if(!b.pod(r.scriptflag) || !b.scr(r.scrconfig)) return false;
	if(!b.pod(r.scripttile) || !b.pod(r.scriptflip)) return false;
	if(!b.pod(r.do_animation) || !b.pod(r.rotation) || !b.pod(r.scale)) return false;
	if(!b.pod(r.moveflags)) return false;
	if(!b.pod(r.drawflags) || !b.pod(r.knockbackflags) || !b.pod(r.screenedge)) return false;
	if(!b.pod(r.scriptshadowtile) || !b.pod(r.knockbackSpeed)) return false;
	if(!b.pod(r.script_knockback_clk) || !b.pod(r.script_knockback_speed)) return false;
	if(!b.pod(r.pit_pulldir) || !b.pod(r.pit_pullclk)) return false;
	if(!b.pod(r.fallclk) || !b.pod(r.fallCombo) || !b.pod(r.o_tile) || !b.pod(r.o_cset)) return false;
	if(!b.pod(r.drownclk) || !b.pod(r.drownCombo)) return false;
	if(!b.pod(r.isspawning) || !b.pod(r.can_flicker) || !b.pod(r.hide_hitbox) || !b.pod(r.behind)) return false;
	if(!b.pod(r.invis_timer)) return false;
	if(!b.pod(r.spr_shadow) || !b.pod(r.spr_death) || !b.pod(r.spr_spawn)) return false;
	if(!b.pod(r.spr_death_anim_clk) || !b.pod(r.spr_spawn_anim_clk)) return false;
	if(!b.pod(r.spr_death_anim_frm) || !b.pod(r.spr_spawn_anim_frm)) return false;
	if(!b.pod(r.glowRad) || !b.pod(r.glowShape) || !b.pod(r.glowOffset)) return false;
	if(!b.pod(r.ignore_delete)) return false;
	if(!b.zf(r.custom_gravity) || !b.zf(r.custom_terminal_v)) return false;
	if(!b.pod(r.viewport_suspend_range) || !b.pod(r.viewport_despawn_range)) return false;
	return true;
}

void apply_sprite(sprite& s, SpriteRec const& r)
{
	s.x=r.x; s.y=r.y; s.vx=r.vx; s.vy=r.vy;
	s.old_x=r.old_x; s.old_y=r.old_y; s.old_x2=r.old_x2; s.old_y2=r.old_y2;
	s.hit_width=r.hit_width; s.hit_height=r.hit_height;
	s.hxofs=r.hxofs; s.hyofs=r.hyofs; s.sxofs=r.sxofs; s.syofs=r.syofs;
	s.sxsz_ofs=r.sxsz_ofs; s.sysz_ofs=r.sysz_ofs; s.solidflags=r.solidflags; s.dir=r.dir;
	s.switch_hooked=r.switch_hooked;
	s.screen_spawned=r.screen_spawned; s.current_screen=r.current_screen;
	s.z=r.z; s.fall=r.fall; s.fakefall=r.fakefall; s.fakez=r.fakez;
	s.tile=r.tile; s.shadowtile=r.shadowtile; s.cs=r.cs; s.flip=r.flip;
	s.c_clk=r.c_clk; s.clk=r.clk; s.misc=r.misc;
	s.flickercolor=r.flickercolor; s.flickertransp=r.flickertransp;
	s.xofs=r.xofs; s.yofs=r.yofs; s.zofs=r.zofs;
	s.shadowxofs=r.shadowxofs; s.shadowyofs=r.shadowyofs;
	s.hzsz=r.hzsz; s.txsz=r.txsz; s.tysz=r.tysz; s.id=r.id;
	s.slopeid=r.slopeid; s.onplatid=r.onplatid;
	s.angular=r.angular; s.can_freeze_from_holdup=r.can_freeze_from_holdup;
	s.is_frozen=r.is_frozen; s.is_within_freeze_viewport=r.is_within_freeze_viewport;
	s.isolated_freeze_viewport=r.isolated_freeze_viewport;
	s.angle=r.angle;
	s.lasthit=r.lasthit; s.lasthitclk=r.lasthitclk;
	for(int i=0;i<10;i++)
	{
		s.dummy_int[i]=r.dummy_int[i]; s.dummy_fix[i]=r.dummy_fix[i];
		s.dummy_float[i]=r.dummy_float[i]; s.dummy_bool[i]=r.dummy_bool[i];
	}
	s.drawstyle=r.drawstyle; s.extend=r.extend;
	for(int i=0;i<32;i++) s.miscellaneous[i]=r.miscellaneous[i];
	s.script_no_colldet=r.script_no_colldet; s.wpnsprite=r.wpnsprite;
	s.scriptflag=r.scriptflag; s.scrconfig=r.scrconfig;
	s.scripttile=r.scripttile; s.scriptflip=r.scriptflip;
	s.do_animation=r.do_animation; s.rotation=r.rotation; s.scale=r.scale;
	s.moveflags=move_flags(r.moveflags);
	s.drawflags=r.drawflags; s.knockbackflags=r.knockbackflags; s.screenedge=r.screenedge;
	s.scriptshadowtile=r.scriptshadowtile; s.knockbackSpeed=r.knockbackSpeed;
	s.script_knockback_clk=r.script_knockback_clk; s.script_knockback_speed=r.script_knockback_speed;
	s.pit_pulldir=r.pit_pulldir; s.pit_pullclk=r.pit_pullclk;
	s.fallclk=r.fallclk; s.fallCombo=r.fallCombo;
	s.o_tile=r.o_tile; s.o_cset=r.o_cset;
	s.drownclk=r.drownclk; s.drownCombo=r.drownCombo;
	s.isspawning=r.isspawning; s.can_flicker=r.can_flicker;
	s.hide_hitbox=r.hide_hitbox; s.behind=r.behind;
	s.invis_timer=r.invis_timer;
	s.spr_shadow=r.spr_shadow; s.spr_death=r.spr_death; s.spr_spawn=r.spr_spawn;
	s.spr_death_anim_clk=r.spr_death_anim_clk; s.spr_spawn_anim_clk=r.spr_spawn_anim_clk;
	s.spr_death_anim_frm=r.spr_death_anim_frm; s.spr_spawn_anim_frm=r.spr_spawn_anim_frm;
	s.glowRad=r.glowRad; s.glowShape=r.glowShape; s.glowOffset=r.glowOffset;
	s.ignore_delete=r.ignore_delete;
	s.custom_gravity=r.custom_gravity; s.custom_terminal_v=r.custom_terminal_v;
	s.viewport_suspend_range=r.viewport_suspend_range;
	s.viewport_despawn_range=r.viewport_despawn_range;
	s.setTempNonsolid(r.temp_nonsolid);
	s.setSolid(r.solid);
}

// ---------------------------------------------------------------------------
// weapon_data (has a script_config, so not raw-copyable)
// ---------------------------------------------------------------------------
struct WeaponDataRec
{
	uint16_t flags=0; uint32_t moveflags=0; uint16_t wflags=0;
	word burnsprs[WPNSPR_MAX]{};
	byte light_rads[WPNSPR_MAX]{};
	int32_t light_offsets[WPNSPR_MAX]{};
	byte glow_shape=0;
	int32_t override_flags=0,hxofs=0,hyofs=0,hxsz=0,hysz=0,hzsz=0,xofs=0,yofs=0,tilew=0,tileh=0;
	zfix step,bounce_mult,bounce_add;
	byte unblockable=0;
	int32_t timeout=0;
	byte imitate_weapon=0,default_defense=0,lift_level=0,lift_time=16;
	zfix lift_height;
	script_config scrconfig;
	int16_t pierce_count=-1;
	int32_t viewport_suspend_range=0,viewport_despawn_range=0;
};

void capture_wdata(weapon_data const& d, WeaponDataRec& r)
{
	r.flags=d.flags; r.moveflags=d.moveflags; r.wflags=d.wflags;
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		r.burnsprs[i]=d.burnsprs[i]; r.light_rads[i]=d.light_rads[i];
		r.light_offsets[i]=d.light_offsets[i];
	}
	r.glow_shape=d.glow_shape;
	r.override_flags=d.override_flags; r.hxofs=d.hxofs; r.hyofs=d.hyofs;
	r.hxsz=d.hxsz; r.hysz=d.hysz; r.hzsz=d.hzsz; r.xofs=d.xofs; r.yofs=d.yofs;
	r.tilew=d.tilew; r.tileh=d.tileh;
	r.step=d.step; r.bounce_mult=d.bounce_mult; r.bounce_add=d.bounce_add;
	r.unblockable=d.unblockable; r.timeout=d.timeout;
	r.imitate_weapon=d.imitate_weapon; r.default_defense=d.default_defense;
	r.lift_level=d.lift_level; r.lift_time=d.lift_time; r.lift_height=d.lift_height;
	r.scrconfig=d.scrconfig; r.pierce_count=d.pierce_count;
	r.viewport_suspend_range=d.viewport_suspend_range;
	r.viewport_despawn_range=d.viewport_despawn_range;
}

void write_wdata(BW& w, WeaponDataRec const& r)
{
	w.pod(r.flags); w.pod(r.moveflags); w.pod(r.wflags);
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		w.pod(r.burnsprs[i]); w.pod(r.light_rads[i]); w.pod(r.light_offsets[i]);
	}
	w.pod(r.glow_shape);
	w.pod(r.override_flags); w.pod(r.hxofs); w.pod(r.hyofs); w.pod(r.hxsz); w.pod(r.hysz);
	w.pod(r.hzsz); w.pod(r.xofs); w.pod(r.yofs); w.pod(r.tilew); w.pod(r.tileh);
	w.zf(r.step); w.zf(r.bounce_mult); w.zf(r.bounce_add);
	w.pod(r.unblockable); w.pod(r.timeout);
	w.pod(r.imitate_weapon); w.pod(r.default_defense);
	w.pod(r.lift_level); w.pod(r.lift_time); w.zf(r.lift_height);
	w.scr(r.scrconfig); w.pod(r.pierce_count);
	w.pod(r.viewport_suspend_range); w.pod(r.viewport_despawn_range);
}

bool read_wdata(BR& b, WeaponDataRec& r)
{
	if(!b.pod(r.flags) || !b.pod(r.moveflags) || !b.pod(r.wflags)) return false;
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		if(!b.pod(r.burnsprs[i]) || !b.pod(r.light_rads[i]) || !b.pod(r.light_offsets[i])) return false;
	}
	if(!b.pod(r.glow_shape)) return false;
	if(!b.pod(r.override_flags) || !b.pod(r.hxofs) || !b.pod(r.hyofs) || !b.pod(r.hxsz) || !b.pod(r.hysz)) return false;
	if(!b.pod(r.hzsz) || !b.pod(r.xofs) || !b.pod(r.yofs) || !b.pod(r.tilew) || !b.pod(r.tileh)) return false;
	if(!b.zf(r.step) || !b.zf(r.bounce_mult) || !b.zf(r.bounce_add)) return false;
	if(!b.pod(r.unblockable) || !b.pod(r.timeout)) return false;
	if(!b.pod(r.imitate_weapon) || !b.pod(r.default_defense)) return false;
	if(!b.pod(r.lift_level) || !b.pod(r.lift_time) || !b.zf(r.lift_height)) return false;
	if(!b.scr(r.scrconfig) || !b.pod(r.pierce_count)) return false;
	if(!b.pod(r.viewport_suspend_range) || !b.pod(r.viewport_despawn_range)) return false;
	return true;
}

void apply_wdata(weapon_data& d, WeaponDataRec const& r)
{
	d.flags=flags::wdata_flags(r.flags); d.moveflags=move_flags(r.moveflags);
	d.wflags=weapon_flags(r.wflags);
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		d.burnsprs[i]=r.burnsprs[i]; d.light_rads[i]=r.light_rads[i];
		d.light_offsets[i]=r.light_offsets[i];
	}
	d.glow_shape=r.glow_shape;
	d.override_flags=r.override_flags; d.hxofs=r.hxofs; d.hyofs=r.hyofs;
	d.hxsz=r.hxsz; d.hysz=r.hysz; d.hzsz=r.hzsz; d.xofs=r.xofs; d.yofs=r.yofs;
	d.tilew=r.tilew; d.tileh=r.tileh;
	d.step=r.step; d.bounce_mult=r.bounce_mult; d.bounce_add=r.bounce_add;
	d.unblockable=r.unblockable; d.timeout=r.timeout;
	d.imitate_weapon=r.imitate_weapon; d.default_defense=r.default_defense;
	d.lift_level=r.lift_level; d.lift_time=r.lift_time; d.lift_height=r.lift_height;
	d.scrconfig=r.scrconfig; d.pierce_count=r.pierce_count;
	d.viewport_suspend_range=r.viewport_suspend_range;
	d.viewport_despawn_range=r.viewport_despawn_range;
}

// ---------------------------------------------------------------------------
// Enemies. Tag = exact subclass, so reconstruction uses the real constructor
// (same one the engine's spawn factory picks) and then overwrites every field.
// ---------------------------------------------------------------------------
enum EnemyTag : int32_t
{
	ET_ENEMY=1, ET_GUY, ET_EFIRE, ET_EOTHER, ET_ESCRIPT, ET_EFRIENDLY,
	ET_EGHINI, ET_ETEKTITE, ET_EITEMFAIRY, ET_EPEAHAT, ET_ELEEVER, ET_EWALLM,
	ET_ETRAP, ET_ETRAP2, ET_EROCK, ET_EBOULDER, ET_EPROJECTILE, ET_ETRIGGER,
	ET_ENPC, ET_ESPINTILE, ET_EZORA, ET_ESTALFOS, ET_EKEESE, ET_EWIZZROBE,
	ET_EDODONGO, ET_EDODONGO2, ET_EAQUAMENTUS, ET_EGOHMA, ET_ELILDIG, ET_EBIGDIG,
	ET_EGANON, ET_EMOLDORM, ET_ESMOLDORM, ET_ELANMOLA, ET_ESLANMOLA,
	ET_EMANHANDLA, ET_ESMANHAN, ET_EGLEEOK, ET_ESGLEEOK, ET_EPATRA, ET_ESPATRA,
	ET_EPATRABS, ET_ESPATRABS
};

struct EnemyRec
{
	SpriteRec s;
	int32_t tag = ET_ENEMY;
	int32_t frate=0,hp=0,hclk=0,clk3=0,stunclk=0,timer=0,fading=0,superman=0,mainguy=0,did_armos=0;
	byte movestatus=0,item_set=0,posframe=0;
	bool itemguy=false,count_enemy=false,dying=false,ceiling=false,leader=false,scored=false,script_spawned=false,dying_despawn=false;
	zfix step,floor_y;
	uint64_t flags=0;
	int16_t type=0,dcset=0,anim=0,dp=0,wdp=0,wpn=0,rate=0,hrate=0,homing=0,grumble=0;
	zfix dstep;
	int32_t dmisc[32]{};
	int16_t bgsfx=0,bosspal=0;
	byte defense[edefLAST255]{};
	word hitsfx=0,deadsfx=0;
	bool submerged=false;
	int32_t clk2=0,sclk=0,starting_hp=0,ox=0,oy=0,s_tile=0;
	int32_t hitby[NUM_HIT_TYPES_USED]{};
	int16_t firesfx=0,firesfx_secondary=0;
	bool isCore=false;
	int16_t parentCore=0;
	int32_t wpnsprite=0,SIZEflags=0,frozentile=0,frozencset=0,frozenclock=0;
	bool hashero=false;
	int16_t frozenmisc[10]{};
	int32_t movement[32]{},new_weapon[32]{};
	byte stickclk=0;
	int32_t editorflags=0;
	bool immortal=false,noSlide=false;
	int16_t hitdir=0;
	int8_t deathexstate=0;
	bool didScriptThisFrame=false;
	WeaponDataRec weap_data;
	bool aim_hero=false;
	word specialsfx=0;
	int32_t summoner_parent_uid=0;
	std::vector<int32_t> summoner_children_uids;
	// Subclass extras (only the ones matching `tag` are (de)serialized).
	int32_t clk4=0,clk5=0,clk6=0,clk7=0,c=0,clk2start=0,cstart=0,fbx=0;
	bool shield=false,fired=false,dashing=false,temprule=false,charging=false,firing=false,adjusted=false;
	int32_t multishot=0,previous_dir=0;
	zfix fy,shadowdistance;
	int32_t Stunclk=0,fakeX=0,fakeY=0,fakeTile=0,AmuletClk=0,AmuletClk2=0;
	int32_t segcnt=0,segid=0,parentclk=0,minRange=0;
	char arm[8]{};
	int32_t armcnt=0;
	int32_t flameclk=0,flamehead=0,necktile=0,headtile=0,flyingheadtile=0;
	int32_t nx[255]{},ny[255]{},nxoffset[255]{},nyoffset[255]{};
	zfix xoffset,yoffset;
	int32_t flycnt=0,flycnt2=0,loopcnt=0,lookat=0;
	double circle_x=0,circle_y=0,temp_x=0,temp_y=0;
	int32_t extra_ox=0,extra_oy=0; // subclass-shadowed ox/oy (eGhini/ePeahat/eKeese/eTrap)
	int32_t ctor_clk=0; // constructor clk argument (segment/head counts, fairy id, ...)
};

int32_t enemy_tag(enemy const* e)
{
	// Most-derived first where inheritance chains exist.
	if(dynamic_cast<guy const*>(e)) return ET_GUY;
	if(dynamic_cast<eFire const*>(e)) return ET_EFIRE;
	if(dynamic_cast<eOther const*>(e)) return ET_EOTHER;
	if(dynamic_cast<eScript const*>(e)) return ET_ESCRIPT;
	if(dynamic_cast<eFriendly const*>(e)) return ET_EFRIENDLY;
	if(dynamic_cast<eGhini const*>(e)) return ET_EGHINI;
	if(dynamic_cast<eTektite const*>(e)) return ET_ETEKTITE;
	if(dynamic_cast<eItemFairy const*>(e)) return ET_EITEMFAIRY;
	if(dynamic_cast<ePeahat const*>(e)) return ET_EPEAHAT;
	if(dynamic_cast<eLeever const*>(e)) return ET_ELEEVER;
	if(dynamic_cast<eWallM const*>(e)) return ET_EWALLM;
	if(dynamic_cast<eTrap2 const*>(e)) return ET_ETRAP2;
	if(dynamic_cast<eTrap const*>(e)) return ET_ETRAP;
	if(dynamic_cast<eBoulder const*>(e)) return ET_EBOULDER;
	if(dynamic_cast<eRock const*>(e)) return ET_EROCK;
	if(dynamic_cast<eProjectile const*>(e)) return ET_EPROJECTILE;
	if(dynamic_cast<eTrigger const*>(e)) return ET_ETRIGGER;
	if(dynamic_cast<eNPC const*>(e)) return ET_ENPC;
	if(dynamic_cast<eSpinTile const*>(e)) return ET_ESPINTILE;
	if(dynamic_cast<eZora const*>(e)) return ET_EZORA;
	if(dynamic_cast<eStalfos const*>(e)) return ET_ESTALFOS;
	if(dynamic_cast<eKeese const*>(e)) return ET_EKEESE;
	if(dynamic_cast<eWizzrobe const*>(e)) return ET_EWIZZROBE;
	if(dynamic_cast<eDodongo2 const*>(e)) return ET_EDODONGO2;
	if(dynamic_cast<eDodongo const*>(e)) return ET_EDODONGO;
	if(dynamic_cast<eAquamentus const*>(e)) return ET_EAQUAMENTUS;
	if(dynamic_cast<eGohma const*>(e)) return ET_EGOHMA;
	if(dynamic_cast<eLilDig const*>(e)) return ET_ELILDIG;
	if(dynamic_cast<eBigDig const*>(e)) return ET_EBIGDIG;
	if(dynamic_cast<eGanon const*>(e)) return ET_EGANON;
	if(dynamic_cast<esMoldorm const*>(e)) return ET_ESMOLDORM;
	if(dynamic_cast<eMoldorm const*>(e)) return ET_EMOLDORM;
	if(dynamic_cast<esLanmola const*>(e)) return ET_ESLANMOLA;
	if(dynamic_cast<eLanmola const*>(e)) return ET_ELANMOLA;
	if(dynamic_cast<esManhandla const*>(e)) return ET_ESMANHAN;
	if(dynamic_cast<eManhandla const*>(e)) return ET_EMANHANDLA;
	if(dynamic_cast<esGleeok const*>(e)) return ET_ESGLEEOK;
	if(dynamic_cast<eGleeok const*>(e)) return ET_EGLEEOK;
	if(dynamic_cast<esPatraBS const*>(e)) return ET_ESPATRABS;
	if(dynamic_cast<ePatraBS const*>(e)) return ET_EPATRABS;
	if(dynamic_cast<esPatra const*>(e)) return ET_ESPATRA;
	if(dynamic_cast<ePatra const*>(e)) return ET_EPATRA;
	return ET_ENEMY;
}

void capture_enemy(enemy const* e, EnemyRec& r)
{
	capture_sprite(*e, r.s);
	r.tag = enemy_tag(e);
	r.frate=e->frate; r.hp=e->hp; r.hclk=e->hclk; r.clk3=e->clk3; r.stunclk=e->stunclk;
	r.timer=e->timer; r.fading=e->fading; r.superman=e->superman; r.mainguy=e->mainguy; r.did_armos=e->did_armos;
	r.movestatus=e->movestatus; r.item_set=e->item_set; r.posframe=e->posframe;
	r.itemguy=e->itemguy; r.count_enemy=e->count_enemy; r.dying=e->dying; r.ceiling=e->ceiling;
	r.leader=e->leader; r.scored=e->scored; r.script_spawned=e->script_spawned; r.dying_despawn=e->dying_despawn;
	r.step=e->step; r.floor_y=e->floor_y;
	r.flags=e->flags;
	r.type=e->type; r.dcset=e->dcset; r.anim=e->anim; r.dp=e->dp; r.wdp=e->wdp; r.wpn=e->wpn;
	r.rate=e->rate; r.hrate=e->hrate; r.homing=e->homing; r.grumble=e->grumble;
	r.dstep=e->dstep;
	int32_t const* dm = &e->dmisc1;
	for(int i=0;i<32;i++) r.dmisc[i]=dm[i];
	r.bgsfx=e->bgsfx; r.bosspal=e->bosspal;
	for(int i=0;i<edefLAST255;i++) r.defense[i]=e->defense[i];
	r.hitsfx=e->hitsfx; r.deadsfx=e->deadsfx; r.submerged=e->submerged;
	r.clk2=e->clk2; r.sclk=e->sclk; r.starting_hp=e->starting_hp; r.ox=e->ox; r.oy=e->oy; r.s_tile=e->s_tile;
	for(int i=0;i<NUM_HIT_TYPES_USED;i++) r.hitby[i]=e->hitby[i];
	r.firesfx=e->firesfx; r.firesfx_secondary=e->firesfx_secondary;
	r.isCore=e->isCore; r.parentCore=e->parentCore;
	r.wpnsprite=e->wpnsprite; r.SIZEflags=e->SIZEflags;
	r.frozentile=e->frozentile; r.frozencset=e->frozencset; r.frozenclock=e->frozenclock;
	r.hashero=e->hashero;
	for(int i=0;i<10;i++) r.frozenmisc[i]=e->frozenmisc[i];
	for(int i=0;i<32;i++) { r.movement[i]=e->movement[i]; r.new_weapon[i]=e->new_weapon[i]; }
	r.stickclk=e->stickclk; r.editorflags=e->editorflags;
	r.immortal=e->immortal; r.noSlide=e->noSlide; r.hitdir=e->hitdir;
	r.deathexstate=e->deathexstate; r.didScriptThisFrame=e->didScriptThisFrame;
	capture_wdata(e->weap_data, r.weap_data);
	r.aim_hero=e->aim_hero; r.specialsfx=e->specialsfx;
	r.summoner_parent_uid = e->summoner_parent ? e->summoner_parent->uid : 0;
	for(enemy* ch : e->summoner_children)
		if(ch) r.summoner_children_uids.push_back(ch->uid);
	r.ctor_clk = e->clk;
	switch(r.tag)
	{
		case ET_GUY: break;
		case ET_EFIRE: case ET_EOTHER: case ET_ESCRIPT: case ET_EFRIENDLY:
		{
			if(auto q = dynamic_cast<eFire const*>(e)) { r.clk4=q->clk4; r.shield=q->shield; }
			else if(auto q = dynamic_cast<eOther const*>(e)) { r.clk4=q->clk4; r.shield=q->shield; }
			else if(auto q = dynamic_cast<eScript const*>(e)) { r.clk4=q->clk4; r.shield=q->shield; }
			else if(auto q = dynamic_cast<eFriendly const*>(e)) { r.clk4=q->clk4; r.shield=q->shield; }
			break;
		}
		case ET_EGHINI:
		{ auto p=static_cast<eGhini const*>(e); r.clk4=p->clk4; r.extra_ox=p->ox; r.extra_oy=p->oy; r.c=p->c; break; }
		case ET_ETEKTITE:
		{ auto p=static_cast<eTektite const*>(e); r.c=p->c; r.clk2start=p->clk2start; r.cstart=p->cstart; break; }
		case ET_EPEAHAT:
		{ auto p=static_cast<ePeahat const*>(e); r.extra_ox=p->ox; r.extra_oy=p->oy; r.c=p->c; break; }
		case ET_ELEEVER:
		{ r.temprule=static_cast<eLeever const*>(e)->temprule; break; }
		case ET_ETRAP:
		{ auto p=static_cast<eTrap const*>(e); r.extra_ox=p->ox; r.extra_oy=p->oy; break; }
		case ET_EPROJECTILE:
		{ r.minRange=static_cast<eProjectile const*>(e)->minRange; r.ctor_clk=r.minRange; break; }
		case ET_ESTALFOS:
		{
			auto p=static_cast<eStalfos const*>(e);
			r.clk4=p->clk4; r.clk5=p->clk5; r.fired=p->fired; r.shield=p->shield;
			r.dashing=p->dashing; r.multishot=p->multishot; r.fy=p->fy; r.shadowdistance=p->shadowdistance;
			break;
		}
		case ET_EKEESE:
		{ auto p=static_cast<eKeese const*>(e); r.extra_ox=p->ox; r.extra_oy=p->oy; r.c=p->c; r.clk4=p->clk4; r.clk5=p->clk5; break; }
		case ET_EWIZZROBE:
		{ auto p=static_cast<eWizzrobe const*>(e); r.charging=p->charging; r.firing=p->firing; r.clk4=p->fclk; break; }
		case ET_EDODONGO2:
		{ r.previous_dir=static_cast<eDodongo2 const*>(e)->previous_dir; break; }
		case ET_EAQUAMENTUS:
		{ auto p=static_cast<eAquamentus const*>(e); r.clk4=p->clk4; r.fbx=p->fbx; break; }
		case ET_EGOHMA:
		{ r.clk4=static_cast<eGohma const*>(e)->clk4; break; }
		case ET_EGANON:
		{
			auto p=static_cast<eGanon const*>(e);
			r.Stunclk=p->Stunclk; r.fakeX=p->fakeX; r.fakeY=p->fakeY; r.fakeTile=p->fakeTile;
			r.AmuletClk=p->AmuletClk; r.AmuletClk2=p->AmuletClk2;
			break;
		}
		case ET_EMOLDORM:
		{ auto p=static_cast<eMoldorm const*>(e); r.segcnt=p->segcnt; r.segid=p->segid; r.ctor_clk=p->segcnt; break; }
		case ET_ESMOLDORM:
		{ r.parentclk=static_cast<esMoldorm const*>(e)->parentclk; break; }
		case ET_ELANMOLA:
		{ r.segcnt=static_cast<eLanmola const*>(e)->segcnt; r.ctor_clk=r.segcnt; break; }
		case ET_EMANHANDLA:
		{
			auto p=static_cast<eManhandla const*>(e);
			for(int i=0;i<8;i++) r.arm[i]=p->arm[i];
			r.armcnt=p->armcnt; r.adjusted=p->adjusted;
			break;
		}
		case ET_EGLEEOK:
		{
			auto p=static_cast<eGleeok const*>(e);
			r.flameclk=p->flameclk; r.flamehead=p->flamehead; r.necktile=p->necktile;
			r.ctor_clk=p->misc; // head count passed through clk at spawn
			break;
		}
		case ET_ESGLEEOK:
		{
			auto p=static_cast<esGleeok const*>(e);
			for(int i=0;i<255;i++)
			{
				r.nx[i]=p->nx[i]; r.ny[i]=p->ny[i];
				r.nxoffset[i]=p->nxoffset[i]; r.nyoffset[i]=p->nyoffset[i];
			}
			r.headtile=p->headtile; r.flyingheadtile=p->flyingheadtile; r.necktile=p->necktile;
			r.xoffset=p->xoffset; r.yoffset=p->yoffset;
			break;
		}
		case ET_EPATRA:
		{
			auto p=static_cast<ePatra const*>(e);
			r.flycnt=p->flycnt; r.flycnt2=p->flycnt2; r.loopcnt=p->loopcnt; r.lookat=p->lookat;
			r.circle_x=p->circle_x; r.circle_y=p->circle_y; r.temp_x=p->temp_x; r.temp_y=p->temp_y;
			r.adjusted=p->adjusted; r.clk4=p->clk4; r.clk5=p->clk5; r.clk6=p->clk6; r.clk7=p->clk7;
			break;
		}
		case ET_ESPATRA:
		{ auto p=static_cast<esPatra const*>(e); r.clk4=p->clk4; r.clk5=p->clk5; break; }
		case ET_EPATRABS:
		{
			auto p=static_cast<ePatraBS const*>(e);
			r.flycnt=p->flycnt; r.flycnt2=p->flycnt2; r.loopcnt=p->loopcnt; r.lookat=p->lookat;
			r.temp_x=p->temp_x; r.temp_y=p->temp_y; r.adjusted=p->adjusted;
			r.clk4=p->clk4; r.clk5=p->clk5;
			break;
		}
		case ET_ESPATRABS:
		{ auto p=static_cast<esPatraBS const*>(e); r.clk4=p->clk4; r.clk5=p->clk5; break; }
		default: break;
	}
}

void write_enemy_extra(BW& w, EnemyRec const& r)
{
	switch(r.tag)
	{
		case ET_EFIRE: case ET_EOTHER: case ET_ESCRIPT: case ET_EFRIENDLY:
			w.pod(r.clk4); w.pod(r.shield); break;
		case ET_EGHINI:
			w.pod(r.clk4); w.pod(r.extra_ox); w.pod(r.extra_oy); w.pod(r.c); break;
		case ET_ETEKTITE:
			w.pod(r.c); w.pod(r.clk2start); w.pod(r.cstart); break;
		case ET_EPEAHAT:
			w.pod(r.extra_ox); w.pod(r.extra_oy); w.pod(r.c); break;
		case ET_ELEEVER:
			w.pod(r.temprule); break;
		case ET_ETRAP:
			w.pod(r.extra_ox); w.pod(r.extra_oy); break;
		case ET_EPROJECTILE:
			w.pod(r.minRange); break;
		case ET_ESTALFOS:
			w.pod(r.clk4); w.pod(r.clk5); w.pod(r.fired); w.pod(r.shield);
			w.pod(r.dashing); w.pod(r.multishot); w.zf(r.fy); w.zf(r.shadowdistance); break;
		case ET_EKEESE:
			w.pod(r.extra_ox); w.pod(r.extra_oy); w.pod(r.c); w.pod(r.clk4); w.pod(r.clk5); break;
		case ET_EWIZZROBE:
			w.pod(r.charging); w.pod(r.firing); w.pod(r.clk4); break;
		case ET_EDODONGO2:
			w.pod(r.previous_dir); break;
		case ET_EAQUAMENTUS:
			w.pod(r.clk4); w.pod(r.fbx); break;
		case ET_EGOHMA:
			w.pod(r.clk4); break;
		case ET_EGANON:
			w.pod(r.Stunclk); w.pod(r.fakeX); w.pod(r.fakeY); w.pod(r.fakeTile);
			w.pod(r.AmuletClk); w.pod(r.AmuletClk2); break;
		case ET_EMOLDORM:
			w.pod(r.segcnt); w.pod(r.segid); break;
		case ET_ESMOLDORM:
			w.pod(r.parentclk); break;
		case ET_ELANMOLA:
			w.pod(r.segcnt); break;
		case ET_EMANHANDLA:
			for(int i=0;i<8;i++) w.pod(r.arm[i]);
			w.pod(r.armcnt); w.pod(r.adjusted); break;
		case ET_EGLEEOK:
			w.pod(r.flameclk); w.pod(r.flamehead); w.pod(r.necktile); break;
		case ET_ESGLEEOK:
			for(int i=0;i<255;i++)
			{
				w.pod(r.nx[i]); w.pod(r.ny[i]); w.pod(r.nxoffset[i]); w.pod(r.nyoffset[i]);
			}
			w.pod(r.headtile); w.pod(r.flyingheadtile); w.pod(r.necktile);
			w.zf(r.xoffset); w.zf(r.yoffset); break;
		case ET_EPATRA:
			w.pod(r.flycnt); w.pod(r.flycnt2); w.pod(r.loopcnt); w.pod(r.lookat);
			w.pod(r.circle_x); w.pod(r.circle_y); w.pod(r.temp_x); w.pod(r.temp_y);
			w.pod(r.adjusted); w.pod(r.clk4); w.pod(r.clk5); w.pod(r.clk6); w.pod(r.clk7); break;
		case ET_ESPATRA:
			w.pod(r.clk4); w.pod(r.clk5); break;
		case ET_EPATRABS:
			w.pod(r.flycnt); w.pod(r.flycnt2); w.pod(r.loopcnt); w.pod(r.lookat);
			w.pod(r.temp_x); w.pod(r.temp_y); w.pod(r.adjusted); w.pod(r.clk4); w.pod(r.clk5); break;
		case ET_ESPATRABS:
			w.pod(r.clk4); w.pod(r.clk5); break;
		default: break;
	}
}

bool read_enemy_extra(BR& b, EnemyRec& r)
{
	switch(r.tag)
	{
		case ET_EFIRE: case ET_EOTHER: case ET_ESCRIPT: case ET_EFRIENDLY:
			return b.pod(r.clk4) && b.pod(r.shield);
		case ET_EGHINI:
			return b.pod(r.clk4) && b.pod(r.extra_ox) && b.pod(r.extra_oy) && b.pod(r.c);
		case ET_ETEKTITE:
			return b.pod(r.c) && b.pod(r.clk2start) && b.pod(r.cstart);
		case ET_EPEAHAT:
			return b.pod(r.extra_ox) && b.pod(r.extra_oy) && b.pod(r.c);
		case ET_ELEEVER:
			return b.pod(r.temprule);
		case ET_ETRAP:
			return b.pod(r.extra_ox) && b.pod(r.extra_oy);
		case ET_EPROJECTILE:
			return b.pod(r.minRange);
		case ET_ESTALFOS:
			return b.pod(r.clk4) && b.pod(r.clk5) && b.pod(r.fired) && b.pod(r.shield)
				&& b.pod(r.dashing) && b.pod(r.multishot) && b.zf(r.fy) && b.zf(r.shadowdistance);
		case ET_EKEESE:
			return b.pod(r.extra_ox) && b.pod(r.extra_oy) && b.pod(r.c) && b.pod(r.clk4) && b.pod(r.clk5);
		case ET_EWIZZROBE:
			return b.pod(r.charging) && b.pod(r.firing) && b.pod(r.clk4);
		case ET_EDODONGO2:
			return b.pod(r.previous_dir);
		case ET_EAQUAMENTUS:
			return b.pod(r.clk4) && b.pod(r.fbx);
		case ET_EGOHMA:
			return b.pod(r.clk4);
		case ET_EGANON:
			return b.pod(r.Stunclk) && b.pod(r.fakeX) && b.pod(r.fakeY) && b.pod(r.fakeTile)
				&& b.pod(r.AmuletClk) && b.pod(r.AmuletClk2);
		case ET_EMOLDORM:
			return b.pod(r.segcnt) && b.pod(r.segid);
		case ET_ESMOLDORM:
			return b.pod(r.parentclk);
		case ET_ELANMOLA:
			return b.pod(r.segcnt);
		case ET_EMANHANDLA:
		{
			for(int i=0;i<8;i++) if(!b.pod(r.arm[i])) return false;
			return b.pod(r.armcnt) && b.pod(r.adjusted);
		}
		case ET_EGLEEOK:
			return b.pod(r.flameclk) && b.pod(r.flamehead) && b.pod(r.necktile);
		case ET_ESGLEEOK:
		{
			for(int i=0;i<255;i++)
			{
				if(!b.pod(r.nx[i]) || !b.pod(r.ny[i]) || !b.pod(r.nxoffset[i]) || !b.pod(r.nyoffset[i]))
					return false;
			}
			return b.pod(r.headtile) && b.pod(r.flyingheadtile) && b.pod(r.necktile)
				&& b.zf(r.xoffset) && b.zf(r.yoffset);
		}
		case ET_EPATRA:
			return b.pod(r.flycnt) && b.pod(r.flycnt2) && b.pod(r.loopcnt) && b.pod(r.lookat)
				&& b.pod(r.circle_x) && b.pod(r.circle_y) && b.pod(r.temp_x) && b.pod(r.temp_y)
				&& b.pod(r.adjusted) && b.pod(r.clk4) && b.pod(r.clk5) && b.pod(r.clk6) && b.pod(r.clk7);
		case ET_ESPATRA:
			return b.pod(r.clk4) && b.pod(r.clk5);
		case ET_EPATRABS:
			return b.pod(r.flycnt) && b.pod(r.flycnt2) && b.pod(r.loopcnt) && b.pod(r.lookat)
				&& b.pod(r.temp_x) && b.pod(r.temp_y) && b.pod(r.adjusted) && b.pod(r.clk4) && b.pod(r.clk5);
		case ET_ESPATRABS:
			return b.pod(r.clk4) && b.pod(r.clk5);
		default: return true;
	}
}

void write_enemy(BW& w, EnemyRec const& r)
{
	write_sprite(w, r.s);
	w.pod(r.tag);
	w.pod(r.frate); w.pod(r.hp); w.pod(r.hclk); w.pod(r.clk3); w.pod(r.stunclk);
	w.pod(r.timer); w.pod(r.fading); w.pod(r.superman); w.pod(r.mainguy); w.pod(r.did_armos);
	w.pod(r.movestatus); w.pod(r.item_set); w.pod(r.posframe);
	w.pod(r.itemguy); w.pod(r.count_enemy); w.pod(r.dying); w.pod(r.ceiling);
	w.pod(r.leader); w.pod(r.scored); w.pod(r.script_spawned); w.pod(r.dying_despawn);
	w.zf(r.step); w.zf(r.floor_y);
	w.pod(r.flags);
	w.pod(r.type); w.pod(r.dcset); w.pod(r.anim); w.pod(r.dp); w.pod(r.wdp); w.pod(r.wpn);
	w.pod(r.rate); w.pod(r.hrate); w.pod(r.homing); w.pod(r.grumble);
	w.zf(r.dstep);
	for(int i=0;i<32;i++) w.pod(r.dmisc[i]);
	w.pod(r.bgsfx); w.pod(r.bosspal);
	for(int i=0;i<edefLAST255;i++) w.pod(r.defense[i]);
	w.pod(r.hitsfx); w.pod(r.deadsfx); w.pod(r.submerged);
	w.pod(r.clk2); w.pod(r.sclk); w.pod(r.starting_hp); w.pod(r.ox); w.pod(r.oy); w.pod(r.s_tile);
	for(int i=0;i<NUM_HIT_TYPES_USED;i++) w.pod(r.hitby[i]);
	w.pod(r.firesfx); w.pod(r.firesfx_secondary);
	w.pod(r.isCore); w.pod(r.parentCore);
	w.pod(r.wpnsprite); w.pod(r.SIZEflags); w.pod(r.frozentile); w.pod(r.frozencset); w.pod(r.frozenclock);
	w.pod(r.hashero);
	for(int i=0;i<10;i++) w.pod(r.frozenmisc[i]);
	for(int i=0;i<32;i++) { w.pod(r.movement[i]); w.pod(r.new_weapon[i]); }
	w.pod(r.stickclk); w.pod(r.editorflags);
	w.pod(r.immortal); w.pod(r.noSlide); w.pod(r.hitdir);
	w.pod(r.deathexstate); w.pod(r.didScriptThisFrame);
	write_wdata(w, r.weap_data);
	w.pod(r.aim_hero); w.pod(r.specialsfx);
	w.pod(r.summoner_parent_uid);
	w.pod(uint32_t(r.summoner_children_uids.size()));
	for(int32_t u : r.summoner_children_uids) w.pod(u);
	w.pod(r.ctor_clk);
	write_enemy_extra(w, r);
}

bool read_enemy(BR& b, EnemyRec& r)
{
	if(!read_sprite(b, r.s)) return false;
	if(!b.pod(r.tag)) return false;
	if(r.tag < ET_ENEMY || r.tag > ET_ESPATRABS) { b.ok=false; return false; }
	if(!b.pod(r.frate) || !b.pod(r.hp) || !b.pod(r.hclk) || !b.pod(r.clk3) || !b.pod(r.stunclk)) return false;
	if(!b.pod(r.timer) || !b.pod(r.fading) || !b.pod(r.superman) || !b.pod(r.mainguy) || !b.pod(r.did_armos)) return false;
	if(!b.pod(r.movestatus) || !b.pod(r.item_set) || !b.pod(r.posframe)) return false;
	if(!b.pod(r.itemguy) || !b.pod(r.count_enemy) || !b.pod(r.dying) || !b.pod(r.ceiling)) return false;
	if(!b.pod(r.leader) || !b.pod(r.scored) || !b.pod(r.script_spawned) || !b.pod(r.dying_despawn)) return false;
	if(!b.zf(r.step) || !b.zf(r.floor_y)) return false;
	if(!b.pod(r.flags)) return false;
	if(!b.pod(r.type) || !b.pod(r.dcset) || !b.pod(r.anim) || !b.pod(r.dp) || !b.pod(r.wdp) || !b.pod(r.wpn)) return false;
	if(!b.pod(r.rate) || !b.pod(r.hrate) || !b.pod(r.homing) || !b.pod(r.grumble)) return false;
	if(!b.zf(r.dstep)) return false;
	for(int i=0;i<32;i++) if(!b.pod(r.dmisc[i])) return false;
	if(!b.pod(r.bgsfx) || !b.pod(r.bosspal)) return false;
	for(int i=0;i<edefLAST255;i++) if(!b.pod(r.defense[i])) return false;
	if(!b.pod(r.hitsfx) || !b.pod(r.deadsfx) || !b.pod(r.submerged)) return false;
	if(!b.pod(r.clk2) || !b.pod(r.sclk) || !b.pod(r.starting_hp) || !b.pod(r.ox) || !b.pod(r.oy) || !b.pod(r.s_tile)) return false;
	for(int i=0;i<NUM_HIT_TYPES_USED;i++) if(!b.pod(r.hitby[i])) return false;
	if(!b.pod(r.firesfx) || !b.pod(r.firesfx_secondary)) return false;
	if(!b.pod(r.isCore) || !b.pod(r.parentCore)) return false;
	if(!b.pod(r.wpnsprite) || !b.pod(r.SIZEflags) || !b.pod(r.frozentile) || !b.pod(r.frozencset) || !b.pod(r.frozenclock)) return false;
	if(!b.pod(r.hashero)) return false;
	for(int i=0;i<10;i++) if(!b.pod(r.frozenmisc[i])) return false;
	for(int i=0;i<32;i++) if(!b.pod(r.movement[i]) || !b.pod(r.new_weapon[i])) return false;
	if(!b.pod(r.stickclk) || !b.pod(r.editorflags)) return false;
	if(!b.pod(r.immortal) || !b.pod(r.noSlide) || !b.pod(r.hitdir)) return false;
	if(!b.pod(r.deathexstate) || !b.pod(r.didScriptThisFrame)) return false;
	if(!read_wdata(b, r.weap_data)) return false;
	if(!b.pod(r.aim_hero) || !b.pod(r.specialsfx)) return false;
	if(!b.pod(r.summoner_parent_uid)) return false;
	uint32_t n; if(!b.pod(n)) return false;
	if(n > 4096) { b.ok=false; return false; }
	r.summoner_children_uids.resize(n);
	for(uint32_t i=0;i<n;i++) if(!b.pod(r.summoner_children_uids[i])) return false;
	if(!b.pod(r.ctor_clk)) return false;
	return read_enemy_extra(b, r);
}

void apply_enemy(enemy* e, EnemyRec const& r)
{
	apply_sprite(*e, r.s);
	e->frate=r.frate; e->hp=r.hp; e->hclk=r.hclk; e->clk3=r.clk3; e->stunclk=r.stunclk;
	e->timer=r.timer; e->fading=r.fading; e->superman=r.superman; e->mainguy=r.mainguy; e->did_armos=r.did_armos;
	e->movestatus=r.movestatus; e->item_set=r.item_set; e->posframe=r.posframe;
	e->itemguy=r.itemguy; e->count_enemy=r.count_enemy; e->dying=r.dying; e->ceiling=r.ceiling;
	e->leader=r.leader; e->scored=r.scored; e->script_spawned=r.script_spawned; e->dying_despawn=r.dying_despawn;
	e->step=r.step; e->floor_y=r.floor_y;
	e->flags=guy_flags(r.flags);
	e->type=r.type; e->dcset=r.dcset; e->anim=r.anim; e->dp=r.dp; e->wdp=r.wdp; e->wpn=r.wpn;
	e->rate=r.rate; e->hrate=r.hrate; e->homing=r.homing; e->grumble=r.grumble;
	e->dstep=r.dstep;
	int32_t* dm = &e->dmisc1;
	for(int i=0;i<32;i++) dm[i]=r.dmisc[i];
	e->bgsfx=r.bgsfx; e->bosspal=r.bosspal;
	for(int i=0;i<edefLAST255;i++) e->defense[i]=r.defense[i];
	e->hitsfx=r.hitsfx; e->deadsfx=r.deadsfx; e->submerged=r.submerged;
	e->clk2=r.clk2; e->sclk=r.sclk; e->starting_hp=r.starting_hp; e->ox=r.ox; e->oy=r.oy; e->s_tile=r.s_tile;
	for(int i=0;i<NUM_HIT_TYPES_USED;i++) e->hitby[i]=r.hitby[i];
	e->firesfx=r.firesfx; e->firesfx_secondary=r.firesfx_secondary;
	e->isCore=r.isCore; e->parentCore=r.parentCore;
	e->wpnsprite=r.wpnsprite; e->SIZEflags=r.SIZEflags;
	e->frozentile=r.frozentile; e->frozencset=r.frozencset; e->frozenclock=r.frozenclock;
	e->hashero=r.hashero;
	for(int i=0;i<10;i++) e->frozenmisc[i]=r.frozenmisc[i];
	for(int i=0;i<32;i++) { e->movement[i]=r.movement[i]; e->new_weapon[i]=r.new_weapon[i]; }
	e->stickclk=r.stickclk; e->editorflags=r.editorflags;
	e->immortal=r.immortal; e->noSlide=r.noSlide; e->hitdir=r.hitdir;
	e->deathexstate=r.deathexstate; e->didScriptThisFrame=r.didScriptThisFrame;
	apply_wdata(e->weap_data, r.weap_data);
	e->aim_hero=r.aim_hero; e->specialsfx=r.specialsfx;
	e->activated_handles.clear(); // transient trigger records: re-init
	e->summoner_parent = nullptr;
	e->summoner_children.clear();
	switch(r.tag)
	{
		case ET_EFIRE:
		{ auto p=static_cast<eFire*>(e); p->clk4=r.clk4; p->shield=r.shield; break; }
		case ET_EOTHER:
		{ auto p=static_cast<eOther*>(e); p->clk4=r.clk4; p->shield=r.shield; break; }
		case ET_ESCRIPT:
		{ auto p=static_cast<eScript*>(e); p->clk4=r.clk4; p->shield=r.shield; break; }
		case ET_EFRIENDLY:
		{ auto p=static_cast<eFriendly*>(e); p->clk4=r.clk4; p->shield=r.shield; break; }
		case ET_EGHINI:
		{ auto p=static_cast<eGhini*>(e); p->clk4=r.clk4; p->ox=r.extra_ox; p->oy=r.extra_oy; p->c=r.c; break; }
		case ET_ETEKTITE:
		{ auto p=static_cast<eTektite*>(e); p->c=r.c; p->clk2start=r.clk2start; p->cstart=r.cstart; break; }
		case ET_EPEAHAT:
		{ auto p=static_cast<ePeahat*>(e); p->ox=r.extra_ox; p->oy=r.extra_oy; p->c=r.c; break; }
		case ET_ELEEVER:
		{ static_cast<eLeever*>(e)->temprule=r.temprule; break; }
		case ET_ETRAP:
		{ auto p=static_cast<eTrap*>(e); p->ox=r.extra_ox; p->oy=r.extra_oy; break; }
		case ET_ESTALFOS:
		{
			auto p=static_cast<eStalfos*>(e);
			p->clk4=r.clk4; p->clk5=r.clk5; p->fired=r.fired; p->shield=r.shield;
			p->dashing=r.dashing; p->multishot=r.multishot; p->fy=r.fy; p->shadowdistance=r.shadowdistance;
			break;
		}
		case ET_EKEESE:
		{ auto p=static_cast<eKeese*>(e); p->ox=r.extra_ox; p->oy=r.extra_oy; p->c=r.c; p->clk4=r.clk4; p->clk5=r.clk5; break; }
		case ET_EWIZZROBE:
		{ auto p=static_cast<eWizzrobe*>(e); p->charging=r.charging; p->firing=r.firing; p->fclk=r.clk4; break; }
		case ET_EDODONGO2:
		{ static_cast<eDodongo2*>(e)->previous_dir=r.previous_dir; break; }
		case ET_EAQUAMENTUS:
		{ auto p=static_cast<eAquamentus*>(e); p->clk4=r.clk4; p->fbx=r.fbx; break; }
		case ET_EGOHMA:
		{ static_cast<eGohma*>(e)->clk4=r.clk4; break; }
		case ET_EGANON:
		{
			auto p=static_cast<eGanon*>(e);
			p->Stunclk=r.Stunclk; p->fakeX=r.fakeX; p->fakeY=r.fakeY; p->fakeTile=r.fakeTile;
			p->AmuletClk=r.AmuletClk; p->AmuletClk2=r.AmuletClk2;
			break;
		}
		case ET_EMOLDORM:
		{ auto p=static_cast<eMoldorm*>(e); p->segcnt=r.segcnt; p->segid=r.segid; break; }
		case ET_ESMOLDORM:
		{ static_cast<esMoldorm*>(e)->parentclk=r.parentclk; break; }
		case ET_ELANMOLA:
		{ static_cast<eLanmola*>(e)->segcnt=r.segcnt; break; }
		case ET_EMANHANDLA:
		{
			auto p=static_cast<eManhandla*>(e);
			for(int i=0;i<8;i++) p->arm[i]=r.arm[i];
			p->armcnt=r.armcnt; p->adjusted=r.adjusted;
			break;
		}
		case ET_EGLEEOK:
		{ auto p=static_cast<eGleeok*>(e); p->flameclk=r.flameclk; p->flamehead=r.flamehead; p->necktile=r.necktile; break; }
		case ET_ESGLEEOK:
		{
			auto p=static_cast<esGleeok*>(e);
			for(int i=0;i<255;i++)
			{
				p->nx[i]=r.nx[i]; p->ny[i]=r.ny[i];
				p->nxoffset[i]=r.nxoffset[i]; p->nyoffset[i]=r.nyoffset[i];
			}
			p->headtile=r.headtile; p->flyingheadtile=r.flyingheadtile; p->necktile=r.necktile;
			p->xoffset=r.xoffset; p->yoffset=r.yoffset;
			break;
		}
		case ET_EPATRA:
		{
			auto p=static_cast<ePatra*>(e);
			p->flycnt=r.flycnt; p->flycnt2=r.flycnt2; p->loopcnt=r.loopcnt; p->lookat=r.lookat;
			p->circle_x=r.circle_x; p->circle_y=r.circle_y; p->temp_x=r.temp_x; p->temp_y=r.temp_y;
			p->adjusted=r.adjusted; p->clk4=r.clk4; p->clk5=r.clk5; p->clk6=r.clk6; p->clk7=r.clk7;
			break;
		}
		case ET_ESPATRA:
		{ auto p=static_cast<esPatra*>(e); p->clk4=r.clk4; p->clk5=r.clk5; break; }
		case ET_EPATRABS:
		{
			auto p=static_cast<ePatraBS*>(e);
			p->flycnt=r.flycnt; p->flycnt2=r.flycnt2; p->loopcnt=r.loopcnt; p->lookat=r.lookat;
			p->temp_x=r.temp_x; p->temp_y=r.temp_y; p->adjusted=r.adjusted; p->clk4=r.clk4; p->clk5=r.clk5;
			break;
		}
		case ET_ESPATRABS:
		{ auto p=static_cast<esPatraBS*>(e); p->clk4=r.clk4; p->clk5=r.clk5; break; }
		default: break;
	}
}

// Reconstruct with the exact subclass constructor. `uidmap` holds sprites
// already rebuilt (parents come first in spawn order). Returns null if a
// required parent is missing (skip rather than crash).
enemy* build_enemy(EnemyRec const& r, std::map<int32_t,sprite*> const& uidmap)
{
	zfix X=r.s.x, Y=r.s.y;
	int32_t Id=r.s.id, Clk=r.ctor_clk;
	sprite* prnt = nullptr;
	if(r.s.parent_uid)
	{
		auto it = uidmap.find(r.s.parent_uid);
		if(it != uidmap.end()) prnt = it->second;
	}
	switch(r.tag)
	{
		case ET_ENEMY: return new enemy(X,Y,Id,Clk);
		case ET_GUY: return new guy(X,Y,Id,Clk,r.mainguy);
		case ET_EFIRE: return new eFire(X,Y,Id,Clk);
		case ET_EOTHER: return new eOther(X,Y,Id,Clk);
		case ET_ESCRIPT: return new eScript(X,Y,Id,Clk);
		case ET_EFRIENDLY: return new eFriendly(X,Y,Id,Clk);
		case ET_EGHINI: return new eGhini(X,Y,Id,Clk);
		case ET_ETEKTITE: return new eTektite(X,Y,Id,Clk);
		case ET_EITEMFAIRY: return new eItemFairy(X,Y,Id,Clk);
		case ET_EPEAHAT: return new ePeahat(X,Y,Id,Clk);
		case ET_ELEEVER: return new eLeever(X,Y,Id,Clk);
		case ET_EWALLM: return new eWallM(X,Y,Id,Clk);
		case ET_ETRAP: return new eTrap(X,Y,Id,Clk);
		case ET_ETRAP2: return new eTrap2(X,Y,Id,Clk);
		case ET_EROCK: return new eRock(X,Y,Id,Clk);
		case ET_EBOULDER: return new eBoulder(X,Y,Id,Clk);
		case ET_EPROJECTILE: return new eProjectile(X,Y,Id,r.minRange);
		case ET_ETRIGGER: return new eTrigger(X,Y,Id,Clk);
		case ET_ENPC: return new eNPC(X,Y,Id,Clk);
		case ET_ESPINTILE: return new eSpinTile(X,Y,Id,Clk);
		case ET_EZORA: return new eZora(X,Y,Id,Clk);
		case ET_ESTALFOS: return new eStalfos(X,Y,Id,Clk);
		case ET_EKEESE: return new eKeese(X,Y,Id,Clk);
		case ET_EWIZZROBE: return new eWizzrobe(X,Y,Id,Clk);
		case ET_EDODONGO: return new eDodongo(X,Y,Id,Clk);
		case ET_EDODONGO2: return new eDodongo2(X,Y,Id,Clk);
		case ET_EAQUAMENTUS: return new eAquamentus(X,Y,Id,Clk);
		case ET_EGOHMA: return new eGohma(X,Y,Id,Clk);
		case ET_ELILDIG: return new eLilDig(X,Y,Id,Clk);
		case ET_EBIGDIG: return new eBigDig(X,Y,Id,Clk);
		case ET_EGANON: return new eGanon(X,Y,Id,Clk);
		case ET_EMOLDORM: return new eMoldorm(X,Y,Id,r.segcnt);
		case ET_ESMOLDORM: return new esMoldorm(X,Y,Id,Clk);
		case ET_ELANMOLA: return new eLanmola(X,Y,Id,r.segcnt);
		case ET_ESLANMOLA: return new esLanmola(X,Y,Id,Clk);
		case ET_EMANHANDLA: return new eManhandla(X,Y,Id,Clk);
		case ET_ESMANHAN: return new esManhandla(X,Y,Id,Clk);
		case ET_EGLEEOK: return new eGleeok(X,Y,Id,Clk);
		case ET_ESGLEEOK:
			if(!prnt) return nullptr; // ctor dereferences parent
			return new esGleeok(X,Y,Id,Clk,prnt);
		case ET_EPATRA: return new ePatra(X,Y,Id,Clk);
		case ET_ESPATRA:
			if(!prnt) return nullptr;
			return new esPatra(X,Y,Id,Clk,prnt);
		case ET_EPATRABS: return new ePatraBS(X,Y,Id,Clk);
		case ET_ESPATRABS:
			if(!prnt) return nullptr;
			return new esPatraBS(X,Y,Id,Clk,prnt);
		default: return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------
struct WeaponRec
{
	SpriteRec s;
	int32_t ctor_level=0,ctor_power=0,ctor_dir=0,ctor_parentitem=-1,ctor_parentid=-1;
	byte ctor_scriptgen=0,ctor_isLW=0,ctor_special=0;
	int32_t ctor_use_sprite=-1;
	bool ctor_autorotate=false;
	int32_t power=0,dead=-1,clk2=0,misc2=0,level=0;
	int32_t ignorecombo=0;
	uint16_t ignoreffc=0;
	bool fake_weapon=false,isLit=false;
	int32_t parentid=-1,parentitem=-1,dragging=-1;
	zfix step;
	bool bounce=false,ignoreHero=false;
	word flash=0,wid=0,aframe=0,csclk=0;
	int32_t o_speed=0,o_type=0,frames=0,o_flip=0,ref_o_tile=0;
	byte script_wrote_otile=0;
	int32_t temp1=0;
	bool autorotate=false;
	byte linkedItem=0,unblockable=0;
	uint16_t misc_wflags=0;
	word misc_wsprites[WPNSPR_MAX]{};
	byte light_rads[WPNSPR_MAX]{};
	int32_t light_offsets[WPNSPR_MAX]{};
	byte last_burnstate=0;
	byte wscreengrid[22]{};
	byte wscreengrid_layer[6][22]{};
	int16_t death_spawnitem=-1,death_spawndropset=-1;
	int32_t death_item_pflags=0;
	int16_t death_sprite=-1;
	word death_sfx=0;
	bool has_shadow=true;
	byte lift_level=0,lift_time=16;
	zfix lift_height;
	int32_t count1=0,count2=0,count3=0,count4=0,count5=0;
	byte useweapon=0,useweapondummy=0,usedefense=0,usedefensedummy=0;
	word linked_parent=0;
	byte quantity_iterator=0;
	int32_t weap_pattern[ITEM_MOVEMENT_PATTERNS]{};
	int32_t weaprange=0,weapduration=0;
	int32_t clocks[WEAPON_CLOCKS]{};
	int32_t tilemod=0;
	byte drawlayer=0;
	word family_class=0;
	byte family_level=0;
	word flags=0;
	int32_t collectflags=0;
	int32_t ffmisc[FFSCRIPT_MISC]{};
	char weapname[128]{};
	int32_t duplicates=0;
	int32_t wpn_misc_d[FFSCRIPT_MISC]{};
	int32_t wpnsprite=0;
	byte ScriptGenerated=0,isLWeapon=0;
	bool weapon_dying_frame=false,weapon_start_frame=true,rundeath=false;
	int32_t weap_timeout=0;
	byte specialinfo=0;
	int16_t pierce_count=-1;
	zfix bounce_mult,bounce_add;
	int32_t shd_aclk=0,shd_aframe=0;
	word specialsfx=0;
	bool disable_triggers=false;
};

void capture_weapon(weapon const* w, WeaponRec& r)
{
	capture_sprite(*w, r.s);
	r.ctor_level=w->level; r.ctor_power=w->power; r.ctor_dir=w->dir;
	r.ctor_parentitem=w->parentitem; r.ctor_parentid=w->parentid;
	r.ctor_scriptgen=w->ScriptGenerated; r.ctor_isLW=w->isLWeapon;
	r.ctor_special=w->specialinfo; r.ctor_use_sprite=w->wpnsprite;
	r.ctor_autorotate=w->autorotate;
	r.power=w->power; r.dead=w->dead; r.clk2=w->clk2; r.misc2=w->misc2; r.level=w->level;
	r.ignorecombo=int32_t(w->ignorecombo); r.ignoreffc=w->ignoreffc;
	r.fake_weapon=w->fake_weapon; r.isLit=w->isLit;
	r.parentid=w->parentid; r.parentitem=w->parentitem; r.dragging=w->dragging;
	r.step=w->step; r.bounce=w->bounce; r.ignoreHero=w->ignoreHero;
	r.flash=w->flash; r.wid=w->wid; r.aframe=w->aframe; r.csclk=w->csclk;
	r.o_speed=w->o_speed; r.o_type=w->o_type; r.frames=w->frames; r.o_flip=w->o_flip; r.ref_o_tile=w->ref_o_tile;
	r.script_wrote_otile=w->script_wrote_otile; r.temp1=w->temp1;
	r.autorotate=w->autorotate; r.linkedItem=w->linkedItem; r.unblockable=w->unblockable;
	r.misc_wflags=w->misc_wflags;
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		r.misc_wsprites[i]=w->misc_wsprites[i]; r.light_rads[i]=w->light_rads[i];
		r.light_offsets[i]=w->light_offsets[i];
	}
	r.last_burnstate=w->last_burnstate;
	for(int i=0;i<22;i++) r.wscreengrid[i]=w->wscreengrid[i];
	for(int l=0;l<6;l++) for(int i=0;i<22;i++) r.wscreengrid_layer[l][i]=w->wscreengrid_layer[l][i];
	r.death_spawnitem=w->death_spawnitem; r.death_spawndropset=w->death_spawndropset;
	r.death_item_pflags=w->death_item_pflags; r.death_sprite=w->death_sprite; r.death_sfx=w->death_sfx;
	r.has_shadow=w->has_shadow;
	r.lift_level=w->lift_level; r.lift_time=w->lift_time; r.lift_height=w->lift_height;
	r.count1=w->count1; r.count2=w->count2; r.count3=w->count3; r.count4=w->count4; r.count5=w->count5;
	r.useweapon=w->useweapon; r.useweapondummy=w->useweapondummy;
	r.usedefense=w->usedefense; r.usedefensedummy=w->usedefensedummy;
	r.linked_parent=w->linked_parent; r.quantity_iterator=w->quantity_iterator;
	for(int i=0;i<ITEM_MOVEMENT_PATTERNS;i++) r.weap_pattern[i]=w->weap_pattern[i];
	r.weaprange=w->weaprange; r.weapduration=w->weapduration;
	for(int i=0;i<WEAPON_CLOCKS;i++) r.clocks[i]=w->clocks[i];
	r.tilemod=w->tilemod; r.drawlayer=w->drawlayer;
	r.family_class=w->family_class; r.family_level=w->family_level;
	r.flags=w->flags; r.collectflags=w->collectflags;
	for(int i=0;i<FFSCRIPT_MISC;i++) { r.ffmisc[i]=w->ffmisc[i]; r.wpn_misc_d[i]=w->wpn_misc_d[i]; }
	memcpy(r.weapname, w->weapname, 128);
	r.duplicates=w->duplicates;
	r.wpnsprite=w->wpnsprite;
	r.ScriptGenerated=w->ScriptGenerated; r.isLWeapon=w->isLWeapon;
	r.weapon_dying_frame=w->weapon_dying_frame; r.weapon_start_frame=w->weapon_start_frame;
	r.rundeath=w->rundeath; r.weap_timeout=w->weap_timeout; r.specialinfo=w->specialinfo;
	r.pierce_count=w->pierce_count; r.bounce_mult=w->bounce_mult; r.bounce_add=w->bounce_add;
	r.shd_aclk=w->shd_aclk; r.shd_aframe=w->shd_aframe; r.specialsfx=w->specialsfx;
	r.disable_triggers=w->disable_triggers;
}

void write_weapon(BW& w, WeaponRec const& r)
{
	write_sprite(w, r.s);
	w.pod(r.ctor_level); w.pod(r.ctor_power); w.pod(r.ctor_dir); w.pod(r.ctor_parentitem); w.pod(r.ctor_parentid);
	w.pod(r.ctor_scriptgen); w.pod(r.ctor_isLW); w.pod(r.ctor_special);
	w.pod(r.ctor_use_sprite); w.pod(r.ctor_autorotate);
	w.pod(r.power); w.pod(r.dead); w.pod(r.clk2); w.pod(r.misc2); w.pod(r.level);
	w.pod(r.ignorecombo); w.pod(r.ignoreffc);
	w.pod(r.fake_weapon); w.pod(r.isLit);
	w.pod(r.parentid); w.pod(r.parentitem); w.pod(r.dragging);
	w.zf(r.step); w.pod(r.bounce); w.pod(r.ignoreHero);
	w.pod(r.flash); w.pod(r.wid); w.pod(r.aframe); w.pod(r.csclk);
	w.pod(r.o_speed); w.pod(r.o_type); w.pod(r.frames); w.pod(r.o_flip); w.pod(r.ref_o_tile);
	w.pod(r.script_wrote_otile); w.pod(r.temp1);
	w.pod(r.autorotate); w.pod(r.linkedItem); w.pod(r.unblockable);
	w.pod(r.misc_wflags);
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		w.pod(r.misc_wsprites[i]); w.pod(r.light_rads[i]); w.pod(r.light_offsets[i]);
	}
	w.pod(r.last_burnstate);
	for(int i=0;i<22;i++) w.pod(r.wscreengrid[i]);
	for(int l=0;l<6;l++) for(int i=0;i<22;i++) w.pod(r.wscreengrid_layer[l][i]);
	w.pod(r.death_spawnitem); w.pod(r.death_spawndropset);
	w.pod(r.death_item_pflags); w.pod(r.death_sprite); w.pod(r.death_sfx);
	w.pod(r.has_shadow);
	w.pod(r.lift_level); w.pod(r.lift_time); w.zf(r.lift_height);
	w.pod(r.count1); w.pod(r.count2); w.pod(r.count3); w.pod(r.count4); w.pod(r.count5);
	w.pod(r.useweapon); w.pod(r.useweapondummy); w.pod(r.usedefense); w.pod(r.usedefensedummy);
	w.pod(r.linked_parent); w.pod(r.quantity_iterator);
	for(int i=0;i<ITEM_MOVEMENT_PATTERNS;i++) w.pod(r.weap_pattern[i]);
	w.pod(r.weaprange); w.pod(r.weapduration);
	for(int i=0;i<WEAPON_CLOCKS;i++) w.pod(r.clocks[i]);
	w.pod(r.tilemod); w.pod(r.drawlayer);
	w.pod(r.family_class); w.pod(r.family_level);
	w.pod(r.flags); w.pod(r.collectflags);
	for(int i=0;i<FFSCRIPT_MISC;i++) { w.pod(r.ffmisc[i]); w.pod(r.wpn_misc_d[i]); }
	w.raw(r.weapname, 128);
	w.pod(r.duplicates);
	w.pod(r.wpnsprite);
	w.pod(r.ScriptGenerated); w.pod(r.isLWeapon);
	w.pod(r.weapon_dying_frame); w.pod(r.weapon_start_frame); w.pod(r.rundeath);
	w.pod(r.weap_timeout); w.pod(r.specialinfo);
	w.pod(r.pierce_count); w.zf(r.bounce_mult); w.zf(r.bounce_add);
	w.pod(r.shd_aclk); w.pod(r.shd_aframe); w.pod(r.specialsfx);
	w.pod(r.disable_triggers);
}

bool read_weapon(BR& b, WeaponRec& r)
{
	if(!read_sprite(b, r.s)) return false;
	if(!b.pod(r.ctor_level) || !b.pod(r.ctor_power) || !b.pod(r.ctor_dir) || !b.pod(r.ctor_parentitem) || !b.pod(r.ctor_parentid)) return false;
	if(!b.pod(r.ctor_scriptgen) || !b.pod(r.ctor_isLW) || !b.pod(r.ctor_special)) return false;
	if(!b.pod(r.ctor_use_sprite) || !b.pod(r.ctor_autorotate)) return false;
	if(!b.pod(r.power) || !b.pod(r.dead) || !b.pod(r.clk2) || !b.pod(r.misc2) || !b.pod(r.level)) return false;
	if(!b.pod(r.ignorecombo) || !b.pod(r.ignoreffc)) return false;
	if(!b.pod(r.fake_weapon) || !b.pod(r.isLit)) return false;
	if(!b.pod(r.parentid) || !b.pod(r.parentitem) || !b.pod(r.dragging)) return false;
	if(!b.zf(r.step) || !b.pod(r.bounce) || !b.pod(r.ignoreHero)) return false;
	if(!b.pod(r.flash) || !b.pod(r.wid) || !b.pod(r.aframe) || !b.pod(r.csclk)) return false;
	if(!b.pod(r.o_speed) || !b.pod(r.o_type) || !b.pod(r.frames) || !b.pod(r.o_flip) || !b.pod(r.ref_o_tile)) return false;
	if(!b.pod(r.script_wrote_otile) || !b.pod(r.temp1)) return false;
	if(!b.pod(r.autorotate) || !b.pod(r.linkedItem) || !b.pod(r.unblockable)) return false;
	if(!b.pod(r.misc_wflags)) return false;
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		if(!b.pod(r.misc_wsprites[i]) || !b.pod(r.light_rads[i]) || !b.pod(r.light_offsets[i])) return false;
	}
	if(!b.pod(r.last_burnstate)) return false;
	for(int i=0;i<22;i++) if(!b.pod(r.wscreengrid[i])) return false;
	for(int l=0;l<6;l++) for(int i=0;i<22;i++) if(!b.pod(r.wscreengrid_layer[l][i])) return false;
	if(!b.pod(r.death_spawnitem) || !b.pod(r.death_spawndropset)) return false;
	if(!b.pod(r.death_item_pflags) || !b.pod(r.death_sprite) || !b.pod(r.death_sfx)) return false;
	if(!b.pod(r.has_shadow)) return false;
	if(!b.pod(r.lift_level) || !b.pod(r.lift_time) || !b.zf(r.lift_height)) return false;
	if(!b.pod(r.count1) || !b.pod(r.count2) || !b.pod(r.count3) || !b.pod(r.count4) || !b.pod(r.count5)) return false;
	if(!b.pod(r.useweapon) || !b.pod(r.useweapondummy) || !b.pod(r.usedefense) || !b.pod(r.usedefensedummy)) return false;
	if(!b.pod(r.linked_parent) || !b.pod(r.quantity_iterator)) return false;
	for(int i=0;i<ITEM_MOVEMENT_PATTERNS;i++) if(!b.pod(r.weap_pattern[i])) return false;
	if(!b.pod(r.weaprange) || !b.pod(r.weapduration)) return false;
	for(int i=0;i<WEAPON_CLOCKS;i++) if(!b.pod(r.clocks[i])) return false;
	if(!b.pod(r.tilemod) || !b.pod(r.drawlayer)) return false;
	if(!b.pod(r.family_class) || !b.pod(r.family_level)) return false;
	if(!b.pod(r.flags) || !b.pod(r.collectflags)) return false;
	for(int i=0;i<FFSCRIPT_MISC;i++) if(!b.pod(r.ffmisc[i]) || !b.pod(r.wpn_misc_d[i])) return false;
	if(!b.raw(r.weapname, 128)) return false;
	if(!b.pod(r.duplicates)) return false;
	if(!b.pod(r.wpnsprite)) return false;
	if(!b.pod(r.ScriptGenerated) || !b.pod(r.isLWeapon)) return false;
	if(!b.pod(r.weapon_dying_frame) || !b.pod(r.weapon_start_frame) || !b.pod(r.rundeath)) return false;
	if(!b.pod(r.weap_timeout) || !b.pod(r.specialinfo)) return false;
	if(!b.pod(r.pierce_count) || !b.zf(r.bounce_mult) || !b.zf(r.bounce_add)) return false;
	if(!b.pod(r.shd_aclk) || !b.pod(r.shd_aframe) || !b.pod(r.specialsfx)) return false;
	if(!b.pod(r.disable_triggers)) return false;
	return true;
}

weapon* build_weapon(WeaponRec const& r)
{
	return new weapon(r.s.x, r.s.y, r.s.z, r.s.id, r.ctor_level, r.ctor_power,
		r.ctor_dir, r.ctor_parentitem, r.ctor_parentid, false,
		r.ctor_scriptgen, r.ctor_isLW, r.ctor_special, r.linked_parent,
		r.ctor_use_sprite, r.ctor_autorotate);
}

void apply_weapon(weapon* w, WeaponRec const& r)
{
	apply_sprite(*w, r.s);
	w->power=r.power; w->dead=r.dead; w->clk2=r.clk2; w->misc2=r.misc2; w->level=r.level;
	w->ignorecombo=rpos_t(r.ignorecombo); w->ignoreffc=r.ignoreffc;
	w->fake_weapon=r.fake_weapon; w->isLit=r.isLit;
	w->parentid=r.parentid; w->parentitem=r.parentitem; w->dragging=r.dragging;
	w->step=r.step; w->bounce=r.bounce; w->ignoreHero=r.ignoreHero;
	w->flash=r.flash; w->wid=r.wid; w->aframe=r.aframe; w->csclk=r.csclk;
	w->o_speed=r.o_speed; w->o_type=r.o_type; w->frames=r.frames; w->o_flip=r.o_flip; w->ref_o_tile=r.ref_o_tile;
	w->script_wrote_otile=r.script_wrote_otile; w->temp1=r.temp1;
	w->autorotate=r.autorotate; w->linkedItem=r.linkedItem; w->unblockable=r.unblockable;
	w->misc_wflags=weapon_flags(r.misc_wflags);
	for(int i=0;i<WPNSPR_MAX;i++)
	{
		w->misc_wsprites[i]=r.misc_wsprites[i]; w->light_rads[i]=r.light_rads[i];
		w->light_offsets[i]=r.light_offsets[i];
	}
	w->last_burnstate=r.last_burnstate;
	for(int i=0;i<22;i++) w->wscreengrid[i]=r.wscreengrid[i];
	for(int l=0;l<6;l++) for(int i=0;i<22;i++) w->wscreengrid_layer[l][i]=r.wscreengrid_layer[l][i];
	w->death_spawnitem=r.death_spawnitem; w->death_spawndropset=r.death_spawndropset;
	w->death_item_pflags=r.death_item_pflags; w->death_sprite=r.death_sprite; w->death_sfx=r.death_sfx;
	w->has_shadow=r.has_shadow;
	w->lift_level=r.lift_level; w->lift_time=r.lift_time; w->lift_height=r.lift_height;
	w->count1=r.count1; w->count2=r.count2; w->count3=r.count3; w->count4=r.count4; w->count5=r.count5;
	w->useweapon=r.useweapon; w->useweapondummy=r.useweapondummy;
	w->usedefense=r.usedefense; w->usedefensedummy=r.usedefensedummy;
	w->linked_parent=r.linked_parent; w->quantity_iterator=r.quantity_iterator;
	for(int i=0;i<ITEM_MOVEMENT_PATTERNS;i++) w->weap_pattern[i]=r.weap_pattern[i];
	w->weaprange=r.weaprange; w->weapduration=r.weapduration;
	for(int i=0;i<WEAPON_CLOCKS;i++) w->clocks[i]=r.clocks[i];
	w->tilemod=r.tilemod; w->drawlayer=r.drawlayer;
	w->family_class=r.family_class; w->family_level=r.family_level;
	w->flags=r.flags; w->collectflags=r.collectflags;
	for(int i=0;i<FFSCRIPT_MISC;i++) { w->ffmisc[i]=r.ffmisc[i]; w->wpn_misc_d[i]=r.wpn_misc_d[i]; }
	memcpy(w->weapname, r.weapname, 128);
	w->duplicates=r.duplicates;
	w->wpnsprite=r.wpnsprite;
	w->ScriptGenerated=r.ScriptGenerated; w->isLWeapon=r.isLWeapon;
	w->weapon_dying_frame=r.weapon_dying_frame; w->weapon_start_frame=r.weapon_start_frame;
	w->rundeath=r.rundeath; w->weap_timeout=r.weap_timeout; w->specialinfo=r.specialinfo;
	w->pierce_count=r.pierce_count; w->bounce_mult=r.bounce_mult; w->bounce_add=r.bounce_add;
	w->shd_aclk=r.shd_aclk; w->shd_aframe=r.shd_aframe; w->specialsfx=r.specialsfx;
	w->disable_triggers=r.disable_triggers;
	// Trigger-check caches (rposes_checked / ffcs_checked) stay empty: they are
	// per-frame collision bookkeeping and re-derive on the next animate().
	w->rposes_checked.clear();
	w->ffcs_checked.clear();
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------
struct ItemRec
{
	SpriteRec s;
	int32_t ctor_pickup=0,ctor_clk=0;
	bool ctor_dummy=false;
	int32_t pickup=0,clk2=0,aclk=0;
	word aframe=0;
	int8_t PriceIndex=-1;
	bool flash=false,twohand=false,anim=false,dummy=false;
	int32_t o_speed=0,o_delay=0,frames=0,fairyUID=0;
	word pstring=0,pickup_string_flags=0;
	int32_t type=0;
	byte lvl=0;
	int32_t linked_parent=0;
	bool is_dragged=false;
	int16_t from_dropset=0;
	int8_t pickupexstate=0;
	bool noSound=false,noHoldSound=false,force_grab=false;
};

void capture_item(item* i, ItemRec& r)
{
	capture_sprite(*i, r.s);
	r.ctor_pickup=i->pickup; r.ctor_clk=i->clk; r.ctor_dummy=i->dummy;
	r.pickup=i->pickup; r.clk2=i->clk2; r.aclk=i->aclk;
	r.aframe=i->aframe; r.PriceIndex=i->PriceIndex;
	r.flash=i->flash; r.twohand=i->twohand; r.anim=i->anim; r.dummy=i->dummy;
	r.o_speed=i->o_speed; r.o_delay=i->o_delay; r.frames=i->frames; r.fairyUID=i->fairyUID;
	r.pstring=i->pstring; r.pickup_string_flags=i->pickup_string_flags;
	r.type=i->type; r.lvl=i->lvl; r.linked_parent=i->linked_parent;
	r.is_dragged=i->is_dragged; r.from_dropset=i->from_dropset;
	r.pickupexstate=i->pickupexstate; r.noSound=i->noSound; r.noHoldSound=i->noHoldSound;
	r.force_grab=i->get_forcegrab();
}

void write_item(BW& w, ItemRec const& r)
{
	write_sprite(w, r.s);
	w.pod(r.ctor_pickup); w.pod(r.ctor_clk); w.pod(r.ctor_dummy);
	w.pod(r.pickup); w.pod(r.clk2); w.pod(r.aclk);
	w.pod(r.aframe); w.pod(r.PriceIndex);
	w.pod(r.flash); w.pod(r.twohand); w.pod(r.anim); w.pod(r.dummy);
	w.pod(r.o_speed); w.pod(r.o_delay); w.pod(r.frames); w.pod(r.fairyUID);
	w.pod(r.pstring); w.pod(r.pickup_string_flags);
	w.pod(r.type); w.pod(r.lvl); w.pod(r.linked_parent);
	w.pod(r.is_dragged); w.pod(r.from_dropset);
	w.pod(r.pickupexstate); w.pod(r.noSound); w.pod(r.noHoldSound); w.pod(r.force_grab);
}

bool read_item(BR& b, ItemRec& r)
{
	if(!read_sprite(b, r.s)) return false;
	if(!b.pod(r.ctor_pickup) || !b.pod(r.ctor_clk) || !b.pod(r.ctor_dummy)) return false;
	if(!b.pod(r.pickup) || !b.pod(r.clk2) || !b.pod(r.aclk)) return false;
	if(!b.pod(r.aframe) || !b.pod(r.PriceIndex)) return false;
	if(!b.pod(r.flash) || !b.pod(r.twohand) || !b.pod(r.anim) || !b.pod(r.dummy)) return false;
	if(!b.pod(r.o_speed) || !b.pod(r.o_delay) || !b.pod(r.frames) || !b.pod(r.fairyUID)) return false;
	if(!b.pod(r.pstring) || !b.pod(r.pickup_string_flags)) return false;
	if(!b.pod(r.type) || !b.pod(r.lvl) || !b.pod(r.linked_parent)) return false;
	if(!b.pod(r.is_dragged) || !b.pod(r.from_dropset)) return false;
	if(!b.pod(r.pickupexstate) || !b.pod(r.noSound) || !b.pod(r.noHoldSound) || !b.pod(r.force_grab)) return false;
	return true;
}

void apply_item(item* i, ItemRec const& r)
{
	apply_sprite(*i, r.s);
	i->pickup=r.pickup; i->clk2=r.clk2; i->aclk=r.aclk;
	i->aframe=r.aframe; i->PriceIndex=r.PriceIndex;
	i->flash=r.flash; i->twohand=r.twohand; i->anim=r.anim; i->dummy=r.dummy;
	i->o_speed=r.o_speed; i->o_delay=r.o_delay; i->frames=r.frames; i->fairyUID=r.fairyUID;
	i->pstring=r.pstring; i->pickup_string_flags=r.pickup_string_flags;
	i->type=r.type; i->lvl=r.lvl; i->linked_parent=r.linked_parent;
	i->is_dragged=r.is_dragged; i->from_dropset=r.from_dropset;
	i->pickupexstate=r.pickupexstate; i->noSound=r.noSound; i->noHoldSound=r.noHoldSound;
	i->set_forcegrab(r.force_grab);
}

// ---------------------------------------------------------------------------
// Decorations / portals
// ---------------------------------------------------------------------------
enum DecoTag : int32_t
{
	DT_DECO=1, DT_COMBOSPR, DT_DBUSH, DT_DFLOWER, DT_DGRASS, DT_DHAMMER,
	DT_DTALLGRASS, DT_DRIPPLES, DT_DHOVER, DT_DDIVINE, DT_CUSTOMWALK
};

struct DecoRec
{
	SpriteRec s;
	int32_t tag=DT_DECO;
	int32_t the_deco_sprite=0,tframes=0,spd=0,ox=0,oy=0,wpnid=0;
	byte bits=0;
};

int32_t deco_tag(decoration const* d)
{
	if(dynamic_cast<customWalkSprite const*>(d)) return DT_CUSTOMWALK;
	if(dynamic_cast<comboSprite const*>(d)) return DT_COMBOSPR;
	if(dynamic_cast<dBushLeaves const*>(d)) return DT_DBUSH;
	if(dynamic_cast<dFlowerClippings const*>(d)) return DT_DFLOWER;
	if(dynamic_cast<dGrassClippings const*>(d)) return DT_DGRASS;
	if(dynamic_cast<dHammerSmack const*>(d)) return DT_DHAMMER;
	if(dynamic_cast<dTallGrass const*>(d)) return DT_DTALLGRASS;
	if(dynamic_cast<dRipples const*>(d)) return DT_DRIPPLES;
	if(dynamic_cast<dHover const*>(d)) return DT_DHOVER;
	if(dynamic_cast<dDivineProtectionShield const*>(d)) return DT_DDIVINE;
	return DT_DECO;
}

void capture_deco(decoration const* d, DecoRec& r)
{
	capture_sprite(*d, r.s);
	r.tag=deco_tag(d);
	r.the_deco_sprite=d->the_deco_sprite;
	switch(r.tag)
	{
		case DT_COMBOSPR:
		{ auto p=static_cast<comboSprite const*>(d); r.tframes=p->tframes; r.spd=p->spd; break; }
		case DT_DBUSH:
		{ auto p=static_cast<dBushLeaves const*>(d); r.ox=p->ox; r.oy=p->oy; break; }
		case DT_DFLOWER:
		{ auto p=static_cast<dFlowerClippings const*>(d); r.ox=p->ox; r.oy=p->oy; break; }
		case DT_DGRASS:
		{ auto p=static_cast<dGrassClippings const*>(d); r.ox=p->ox; r.oy=p->oy; break; }
		case DT_DHAMMER:
		{ auto p=static_cast<dHammerSmack const*>(d); r.ox=p->ox; r.oy=p->oy; r.wpnid=p->wpnid; break; }
		case DT_CUSTOMWALK:
		{
			auto p=static_cast<customWalkSprite const*>(d);
			r.tframes=p->tframes; r.spd=p->spd; r.bits=p->bits;
			break;
		}
		default: break;
	}
}

void write_deco(BW& w, DecoRec const& r)
{
	write_sprite(w, r.s);
	w.pod(r.tag); w.pod(r.the_deco_sprite);
	w.pod(r.tframes); w.pod(r.spd); w.pod(r.ox); w.pod(r.oy); w.pod(r.wpnid); w.pod(r.bits);
}

bool read_deco(BR& b, DecoRec& r)
{
	if(!read_sprite(b, r.s)) return false;
	if(!b.pod(r.tag) || !b.pod(r.the_deco_sprite)) return false;
	if(r.tag < DT_DECO || r.tag > DT_CUSTOMWALK) { b.ok=false; return false; }
	if(!b.pod(r.tframes) || !b.pod(r.spd) || !b.pod(r.ox) || !b.pod(r.oy) || !b.pod(r.wpnid) || !b.pod(r.bits)) return false;
	return true;
}

decoration* build_deco(DecoRec const& r)
{
	zfix X=r.s.x, Y=r.s.y;
	int32_t Id=r.s.id, Clk=r.s.clk, Spr=r.the_deco_sprite;
	switch(r.tag)
	{
		case DT_COMBOSPR: return new comboSprite(X,Y,Id,Clk,Spr);
		case DT_DBUSH: return new dBushLeaves(X,Y,Id,Clk,Spr);
		case DT_DFLOWER: return new dFlowerClippings(X,Y,Id,Clk,Spr);
		case DT_DGRASS: return new dGrassClippings(X,Y,Id,Clk,Spr);
		case DT_DHAMMER: return new dHammerSmack(X,Y,Id,Clk,Spr);
		case DT_DTALLGRASS: return new dTallGrass(X,Y,Id,Clk,Spr);
		case DT_DRIPPLES: return new dRipples(X,Y,Id,Clk,Spr);
		case DT_DHOVER: return new dHover(X,Y,Id,Clk,Spr);
		case DT_DDIVINE: return new dDivineProtectionShield(X,Y,Id,Clk);
		case DT_CUSTOMWALK: return new customWalkSprite(X,Y,Id,Clk,Spr);
		default: return new decoration(X,Y,Id,Clk,Spr);
	}
}

void apply_deco(decoration* d, DecoRec const& r)
{
	apply_sprite(*d, r.s);
	d->the_deco_sprite=r.the_deco_sprite;
	switch(r.tag)
	{
		case DT_COMBOSPR:
		{ auto p=static_cast<comboSprite*>(d); p->tframes=r.tframes; p->spd=r.spd; break; }
		case DT_DBUSH:
		{ auto p=static_cast<dBushLeaves*>(d); p->ox=r.ox; p->oy=r.oy; break; }
		case DT_DFLOWER:
		{ auto p=static_cast<dFlowerClippings*>(d); p->ox=r.ox; p->oy=r.oy; break; }
		case DT_DGRASS:
		{ auto p=static_cast<dGrassClippings*>(d); p->ox=r.ox; p->oy=r.oy; break; }
		case DT_DHAMMER:
		{ auto p=static_cast<dHammerSmack*>(d); p->ox=r.ox; p->oy=r.oy; p->wpnid=r.wpnid; break; }
		case DT_CUSTOMWALK:
		{ auto p=static_cast<customWalkSprite*>(d); p->tframes=r.tframes; p->spd=r.spd; p->bits=r.bits; break; }
		default: break;
	}
}

struct PortalRec
{
	SpriteRec s;
	int32_t destdmap=-1,destscr=0,weffect=0,wsfx=0,aclk=0,aframe=0,aspd=0,frames=0,saved_data=0;
	bool prox_active=false;
};

void capture_portal(portal const* p, PortalRec& r)
{
	capture_sprite(*p, r.s);
	r.destdmap=p->destdmap; r.destscr=p->destscr; r.weffect=p->weffect; r.wsfx=p->wsfx;
	r.aclk=p->aclk; r.aframe=p->aframe; r.aspd=p->aspd; r.frames=p->frames;
	r.saved_data=p->saved_data; r.prox_active=p->prox_active;
}

void write_portal(BW& w, PortalRec const& r)
{
	write_sprite(w, r.s);
	w.pod(r.destdmap); w.pod(r.destscr); w.pod(r.weffect); w.pod(r.wsfx);
	w.pod(r.aclk); w.pod(r.aframe); w.pod(r.aspd); w.pod(r.frames);
	w.pod(r.saved_data); w.pod(r.prox_active);
}

bool read_portal(BR& b, PortalRec& r)
{
	if(!read_sprite(b, r.s)) return false;
	if(!b.pod(r.destdmap) || !b.pod(r.destscr) || !b.pod(r.weffect) || !b.pod(r.wsfx)) return false;
	if(!b.pod(r.aclk) || !b.pod(r.aframe) || !b.pod(r.aspd) || !b.pod(r.frames)) return false;
	if(!b.pod(r.saved_data) || !b.pod(r.prox_active)) return false;
	return true;
}

void apply_portal(portal* p, PortalRec const& r)
{
	apply_sprite(*p, r.s);
	p->destdmap=r.destdmap; p->destscr=r.destscr; p->weffect=r.weffect; p->wsfx=r.wsfx;
	p->aclk=r.aclk; p->aframe=r.aframe; p->aspd=r.aspd; p->frames=r.frames;
	p->saved_data=r.saved_data; p->prox_active=r.prox_active;
}

// ---------------------------------------------------------------------------
// Moving block (pushed blocks)
// ---------------------------------------------------------------------------
struct MBlockRec
{
	bool was_active=false;
	SpriteRec s;
	int32_t bcombo=0,oldflag=0,oldcset=0,endx=0,endy=0;
	bool trigger=false,bhole=false;
	byte undercset=0,blockLayer=0;
	zfix step;
	bool force_many=false,no_icy=false,new_block=false,grav_falling=false;
	int32_t bi_data=0;
	word bi_type_clk=0;
	byte bi_pushes[4]{};
	bool bi_crumbling=false,bi_appeared=false;
	word bi_sfx_onchange=0,bi_spr_onchange=0;
};

void capture_mblock(movingblock const& m, MBlockRec& r)
{
	r.was_active=m.active();
	capture_sprite(m, r.s);
	r.bcombo=m.bcombo; r.oldflag=m.oldflag; r.oldcset=m.oldcset;
	r.endx=m.endx; r.endy=m.endy;
	r.trigger=m.trigger; r.bhole=m.bhole;
	r.undercset=m.undercset; r.blockLayer=m.blockLayer;
	r.step=m.step;
	r.force_many=m.force_many; r.no_icy=m.no_icy; r.new_block=m.new_block; r.grav_falling=m.grav_falling;
	r.bi_data=m.blockinfo.data; r.bi_type_clk=m.blockinfo.type_clk;
	for(int i=0;i<4;i++) r.bi_pushes[i]=m.blockinfo.pushes[i];
	r.bi_crumbling=m.blockinfo.crumbling; r.bi_appeared=m.blockinfo.appeared;
	r.bi_sfx_onchange=m.blockinfo.sfx_onchange; r.bi_spr_onchange=m.blockinfo.spr_onchange;
}

void write_mblock(BW& w, MBlockRec const& r)
{
	w.pod(r.was_active);
	write_sprite(w, r.s);
	w.pod(r.bcombo); w.pod(r.oldflag); w.pod(r.oldcset); w.pod(r.endx); w.pod(r.endy);
	w.pod(r.trigger); w.pod(r.bhole);
	w.pod(r.undercset); w.pod(r.blockLayer);
	w.zf(r.step);
	w.pod(r.force_many); w.pod(r.no_icy); w.pod(r.new_block); w.pod(r.grav_falling);
	w.pod(r.bi_data); w.pod(r.bi_type_clk);
	for(int i=0;i<4;i++) w.pod(r.bi_pushes[i]);
	w.pod(r.bi_crumbling); w.pod(r.bi_appeared);
	w.pod(r.bi_sfx_onchange); w.pod(r.bi_spr_onchange);
}

bool read_mblock(BR& b, MBlockRec& r)
{
	if(!b.pod(r.was_active)) return false;
	if(!read_sprite(b, r.s)) return false;
	if(!b.pod(r.bcombo) || !b.pod(r.oldflag) || !b.pod(r.oldcset) || !b.pod(r.endx) || !b.pod(r.endy)) return false;
	if(!b.pod(r.trigger) || !b.pod(r.bhole)) return false;
	if(!b.pod(r.undercset) || !b.pod(r.blockLayer)) return false;
	if(!b.zf(r.step)) return false;
	if(!b.pod(r.force_many) || !b.pod(r.no_icy) || !b.pod(r.new_block) || !b.pod(r.grav_falling)) return false;
	if(!b.pod(r.bi_data) || !b.pod(r.bi_type_clk)) return false;
	for(int i=0;i<4;i++) if(!b.pod(r.bi_pushes[i])) return false;
	if(!b.pod(r.bi_crumbling) || !b.pod(r.bi_appeared)) return false;
	if(!b.pod(r.bi_sfx_onchange) || !b.pod(r.bi_spr_onchange)) return false;
	return true;
}

void apply_mblock(movingblock& m, MBlockRec const& r)
{
	m.clear();
	if(!r.was_active) return;
	apply_sprite(m, r.s);
	m.bcombo=r.bcombo; m.oldflag=r.oldflag; m.oldcset=r.oldcset;
	m.endx=r.endx; m.endy=r.endy;
	m.trigger=r.trigger; m.bhole=r.bhole;
	m.undercset=r.undercset; m.blockLayer=r.blockLayer;
	m.step=r.step;
	m.force_many=r.force_many; m.no_icy=r.no_icy; m.new_block=r.new_block; m.grav_falling=r.grav_falling;
	m.blockinfo.clear();
	m.blockinfo.data=r.bi_data; m.blockinfo.type_clk=r.bi_type_clk;
	for(int i=0;i<4;i++) m.blockinfo.pushes[i]=r.bi_pushes[i];
	m.blockinfo.crumbling=r.bi_crumbling; m.blockinfo.appeared=r.bi_appeared;
	m.blockinfo.sfx_onchange=r.bi_sfx_onchange; m.blockinfo.spr_onchange=r.bi_spr_onchange;
	// blockinfo.trig_data (trigger cooldowns) re-initializes empty.
}

// ---------------------------------------------------------------------------
// FFCs
// ---------------------------------------------------------------------------
struct FfcRec
{
	SpriteRec s;
	word data=0;
	uint32_t fflags=0;
	uint16_t index=0;
	zfix ax,ay;
	word delay=0;
	byte cset=0;
	uint16_t link=0;
	script_config fscrconfig;
	bool hooked=false,recently_hit=false;
	int32_t changer_x=-1000,changer_y=-1000,prev_changer_x=-10000000,prev_changer_y=-10000000;
	bool script_wait_draw=false;
	int8_t layer=1;
	bool loaded=false;
};

void capture_ffc(ffcdata const& f, FfcRec& r)
{
	capture_sprite(f, r.s);
	r.data=f.data; r.fflags=f.flags; r.index=f.index;
	r.ax=f.ax; r.ay=f.ay; r.delay=f.delay; r.cset=f.cset; r.link=f.link;
	r.fscrconfig=f.scrconfig;
	r.hooked=f.hooked; r.recently_hit=f.recently_hit;
	r.changer_x=f.changer_x; r.changer_y=f.changer_y;
	r.prev_changer_x=f.prev_changer_x; r.prev_changer_y=f.prev_changer_y;
	r.script_wait_draw=f.script_wait_draw; r.layer=f.layer;
	r.loaded=f.getLoaded();
}

void write_ffc(BW& w, FfcRec const& r)
{
	write_sprite(w, r.s);
	w.pod(r.data); w.pod(r.fflags); w.pod(r.index);
	w.zf(r.ax); w.zf(r.ay); w.pod(r.delay); w.pod(r.cset); w.pod(r.link);
	w.scr(r.fscrconfig);
	w.pod(r.hooked); w.pod(r.recently_hit);
	w.pod(r.changer_x); w.pod(r.changer_y); w.pod(r.prev_changer_x); w.pod(r.prev_changer_y);
	w.pod(r.script_wait_draw); w.pod(r.layer); w.pod(r.loaded);
}

bool read_ffc(BR& b, FfcRec& r)
{
	if(!read_sprite(b, r.s)) return false;
	if(!b.pod(r.data) || !b.pod(r.fflags) || !b.pod(r.index)) return false;
	if(!b.zf(r.ax) || !b.zf(r.ay) || !b.pod(r.delay) || !b.pod(r.cset) || !b.pod(r.link)) return false;
	if(!b.scr(r.fscrconfig)) return false;
	if(!b.pod(r.hooked) || !b.pod(r.recently_hit)) return false;
	if(!b.pod(r.changer_x) || !b.pod(r.changer_y) || !b.pod(r.prev_changer_x) || !b.pod(r.prev_changer_y)) return false;
	if(!b.pod(r.script_wait_draw) || !b.pod(r.layer) || !b.pod(r.loaded)) return false;
	if(r.index >= MAXFFCS) { b.ok=false; return false; }
	return true;
}

void apply_ffc(ffcdata& f, FfcRec const& r)
{
	apply_sprite(f, r.s);
	f.data=r.data; f.flags=flags::ffc_flags(r.fflags); f.index=r.index;
	f.ax=r.ax; f.ay=r.ay; f.delay=r.delay; f.cset=r.cset; f.link=r.link;
	f.scrconfig=r.fscrconfig;
	f.hooked=r.hooked; f.recently_hit=r.recently_hit;
	f.changer_x=r.changer_x; f.changer_y=r.changer_y;
	f.prev_changer_x=r.prev_changer_x; f.prev_changer_y=r.prev_changer_y;
	f.script_wait_draw=r.script_wait_draw; f.layer=r.layer;
	f.setLoaded(r.loaded);
	f.updateSolid();
}

// ---------------------------------------------------------------------------
// Screen combo buffer + mutable mapscr state
// ---------------------------------------------------------------------------
struct ScreenRec
{
	int32_t screen=0, layer=0;
	byte map=0,valid=0,guy=0;
	int32_t guytile=-1;
	byte guycs=0;
	word roomflags=0,str=0;
	byte room=0;
	word item=0;
	byte hasitem=0;
	byte tilewarptype[4]{};
	byte tilewarpoverlayflags=0;
	word door_combo_set=0;
	byte warpreturnx[4]{},warpreturny[4]{};
	word warpreturnc=0;
	byte stairx=0,stairy=0,itemx=0,itemy=0;
	word color=0;
	byte door[4]{};
	word tilewarpdmap[4]{};
	byte tilewarpscr[4]{};
	byte exitdir=0;
	word enemy[10]{};
	byte pattern=0;
	byte sidewarptype[4]{};
	byte sidewarpoverlayflags=0;
	byte warparrivalx=0,warparrivaly=0;
	byte path[4]{};
	byte maze_transition_wipe=0;
	byte sidewarpscr[4]{};
	word sidewarpdmap[4]{};
	byte sidewarpindex=0;
	word undercombo=0;
	byte undercset=0;
	word catchall=0;
	byte flags=0,flags2=0,flags3=0,flags4=0,flags5=0,flags6=0,flags7=0,flags8=0,flags9=0,flags10=0,flags11=0;
	byte csensitive=1;
	dword noreset=0,nocarry=0,exstate_reset=0,exstate_carry=0;
	byte layermap[6]{},layerscreen[6]{},layeropacity[6]{};
	word timedwarptics=0;
	byte nextmap=0,nextscr=0;
	word secretcombo[128]{};
	byte secretcset[128]{},secretflag[128]{};
	word data[176]{};
	byte sflag[176]{},cset[176]{};
	byte entry_x=0,entry_y=0;
	word oceansfx=0,bosssfx=0,secretsfx=0,holdupsfx=0;
	int32_t music=-1;
	byte lens_layer=0,lens_show=0,lens_hide=0;
	byte preloadscript=0,hidelayers=0,hidescriptlayers=0;
	zfix screen_gravity,screen_terminal_v;
	std::vector<FfcRec> ffcs; // base layer only
};

void capture_screen(mapscr const& s, int32_t screen, int32_t layer, ScreenRec& r)
{
	r.screen=screen; r.layer=layer;
	r.map=s.map; r.valid=s.valid; r.guy=s.guy; r.guytile=s.guytile; r.guycs=s.guycs;
	r.roomflags=s.roomflags; r.str=s.str; r.room=s.room;
	r.item=s.item; r.hasitem=s.hasitem;
	for(int i=0;i<4;i++)
	{
		r.tilewarptype[i]=s.tilewarptype[i];
		r.warpreturnx[i]=s.warpreturnx[i]; r.warpreturny[i]=s.warpreturny[i];
		r.door[i]=s.door[i]; r.tilewarpdmap[i]=s.tilewarpdmap[i]; r.tilewarpscr[i]=s.tilewarpscr[i];
		r.sidewarptype[i]=s.sidewarptype[i]; r.path[i]=s.path[i];
		r.sidewarpscr[i]=s.sidewarpscr[i]; r.sidewarpdmap[i]=s.sidewarpdmap[i];
	}
	r.tilewarpoverlayflags=s.tilewarpoverlayflags; r.door_combo_set=s.door_combo_set;
	r.warpreturnc=s.warpreturnc;
	r.stairx=s.stairx; r.stairy=s.stairy; r.itemx=s.itemx; r.itemy=s.itemy;
	r.color=s.color; r.exitdir=s.exitdir;
	for(int i=0;i<10;i++) r.enemy[i]=s.enemy[i];
	r.pattern=s.pattern;
	r.sidewarpoverlayflags=s.sidewarpoverlayflags;
	r.warparrivalx=s.warparrivalx; r.warparrivaly=s.warparrivaly;
	r.maze_transition_wipe=s.maze_transition_wipe; r.sidewarpindex=s.sidewarpindex;
	r.undercombo=s.undercombo; r.undercset=s.undercset; r.catchall=s.catchall;
	r.flags=s.flags; r.flags2=s.flags2; r.flags3=s.flags3; r.flags4=s.flags4;
	r.flags5=s.flags5; r.flags6=s.flags6; r.flags7=s.flags7; r.flags8=s.flags8;
	r.flags9=s.flags9; r.flags10=s.flags10; r.flags11=s.flags11;
	r.csensitive=s.csensitive;
	r.noreset=s.noreset; r.nocarry=s.nocarry; r.exstate_reset=s.exstate_reset; r.exstate_carry=s.exstate_carry;
	for(int i=0;i<6;i++)
	{
		r.layermap[i]=s.layermap[i]; r.layerscreen[i]=s.layerscreen[i]; r.layeropacity[i]=s.layeropacity[i];
	}
	r.timedwarptics=s.timedwarptics; r.nextmap=s.nextmap; r.nextscr=s.nextscr;
	for(int i=0;i<128;i++)
	{
		r.secretcombo[i]=s.secretcombo[i]; r.secretcset[i]=s.secretcset[i]; r.secretflag[i]=s.secretflag[i];
	}
	for(int i=0;i<176;i++)
	{
		r.data[i]=s.data[i]; r.sflag[i]=s.sflag[i]; r.cset[i]=s.cset[i];
	}
	r.entry_x=s.entry_x; r.entry_y=s.entry_y;
	r.oceansfx=s.oceansfx; r.bosssfx=s.bosssfx; r.secretsfx=s.secretsfx; r.holdupsfx=s.holdupsfx;
	r.music=s.music;
	r.lens_layer=s.lens_layer; r.lens_show=s.lens_show; r.lens_hide=s.lens_hide;
	r.preloadscript=s.preloadscript; r.hidelayers=s.hidelayers; r.hidescriptlayers=s.hidescriptlayers;
	r.screen_gravity=s.screen_gravity; r.screen_terminal_v=s.screen_terminal_v;
	if(layer==0)
	{
		r.ffcs.resize(s.ffcs.size());
		for(size_t i=0;i<s.ffcs.size();i++) capture_ffc(s.ffcs[i], r.ffcs[i]);
	}
}

void write_screen(BW& w, ScreenRec const& r)
{
	w.pod(r.screen); w.pod(r.layer);
	w.pod(r.map); w.pod(r.valid); w.pod(r.guy); w.pod(r.guytile); w.pod(r.guycs);
	w.pod(r.roomflags); w.pod(r.str); w.pod(r.room);
	w.pod(r.item); w.pod(r.hasitem);
	for(int i=0;i<4;i++)
	{
		w.pod(r.tilewarptype[i]); w.pod(r.warpreturnx[i]); w.pod(r.warpreturny[i]);
		w.pod(r.door[i]); w.pod(r.tilewarpdmap[i]); w.pod(r.tilewarpscr[i]);
		w.pod(r.sidewarptype[i]); w.pod(r.path[i]); w.pod(r.sidewarpscr[i]); w.pod(r.sidewarpdmap[i]);
	}
	w.pod(r.tilewarpoverlayflags); w.pod(r.door_combo_set); w.pod(r.warpreturnc);
	w.pod(r.stairx); w.pod(r.stairy); w.pod(r.itemx); w.pod(r.itemy);
	w.pod(r.color); w.pod(r.exitdir);
	for(int i=0;i<10;i++) w.pod(r.enemy[i]);
	w.pod(r.pattern);
	w.pod(r.sidewarpoverlayflags);
	w.pod(r.warparrivalx); w.pod(r.warparrivaly);
	w.pod(r.maze_transition_wipe); w.pod(r.sidewarpindex);
	w.pod(r.undercombo); w.pod(r.undercset); w.pod(r.catchall);
	w.pod(r.flags); w.pod(r.flags2); w.pod(r.flags3); w.pod(r.flags4);
	w.pod(r.flags5); w.pod(r.flags6); w.pod(r.flags7); w.pod(r.flags8);
	w.pod(r.flags9); w.pod(r.flags10); w.pod(r.flags11);
	w.pod(r.csensitive);
	w.pod(r.noreset); w.pod(r.nocarry); w.pod(r.exstate_reset); w.pod(r.exstate_carry);
	for(int i=0;i<6;i++)
	{
		w.pod(r.layermap[i]); w.pod(r.layerscreen[i]); w.pod(r.layeropacity[i]);
	}
	w.pod(r.timedwarptics); w.pod(r.nextmap); w.pod(r.nextscr);
	for(int i=0;i<128;i++)
	{
		w.pod(r.secretcombo[i]); w.pod(r.secretcset[i]); w.pod(r.secretflag[i]);
	}
	for(int i=0;i<176;i++)
	{
		w.pod(r.data[i]); w.pod(r.sflag[i]); w.pod(r.cset[i]);
	}
	w.pod(r.entry_x); w.pod(r.entry_y);
	w.pod(r.oceansfx); w.pod(r.bosssfx); w.pod(r.secretsfx); w.pod(r.holdupsfx);
	w.pod(r.music);
	w.pod(r.lens_layer); w.pod(r.lens_show); w.pod(r.lens_hide);
	w.pod(r.preloadscript); w.pod(r.hidelayers); w.pod(r.hidescriptlayers);
	w.zf(r.screen_gravity); w.zf(r.screen_terminal_v);
	w.pod(uint32_t(r.ffcs.size()));
	for(auto const& f : r.ffcs) write_ffc(w, f);
}

bool read_screen(BR& b, ScreenRec& r)
{
	if(!b.pod(r.screen) || !b.pod(r.layer)) return false;
	if(r.screen < 0 || r.screen > 255 || r.layer < 0 || r.layer > 6) { b.ok=false; return false; }
	if(!b.pod(r.map) || !b.pod(r.valid) || !b.pod(r.guy) || !b.pod(r.guytile) || !b.pod(r.guycs)) return false;
	if(!b.pod(r.roomflags) || !b.pod(r.str) || !b.pod(r.room)) return false;
	if(!b.pod(r.item) || !b.pod(r.hasitem)) return false;
	for(int i=0;i<4;i++)
	{
		if(!b.pod(r.tilewarptype[i]) || !b.pod(r.warpreturnx[i]) || !b.pod(r.warpreturny[i])) return false;
		if(!b.pod(r.door[i]) || !b.pod(r.tilewarpdmap[i]) || !b.pod(r.tilewarpscr[i])) return false;
		if(!b.pod(r.sidewarptype[i]) || !b.pod(r.path[i]) || !b.pod(r.sidewarpscr[i]) || !b.pod(r.sidewarpdmap[i])) return false;
	}
	if(!b.pod(r.tilewarpoverlayflags) || !b.pod(r.door_combo_set) || !b.pod(r.warpreturnc)) return false;
	if(!b.pod(r.stairx) || !b.pod(r.stairy) || !b.pod(r.itemx) || !b.pod(r.itemy)) return false;
	if(!b.pod(r.color) || !b.pod(r.exitdir)) return false;
	for(int i=0;i<10;i++) if(!b.pod(r.enemy[i])) return false;
	if(!b.pod(r.pattern)) return false;
	if(!b.pod(r.sidewarpoverlayflags)) return false;
	if(!b.pod(r.warparrivalx) || !b.pod(r.warparrivaly)) return false;
	if(!b.pod(r.maze_transition_wipe) || !b.pod(r.sidewarpindex)) return false;
	if(!b.pod(r.undercombo) || !b.pod(r.undercset) || !b.pod(r.catchall)) return false;
	if(!b.pod(r.flags) || !b.pod(r.flags2) || !b.pod(r.flags3) || !b.pod(r.flags4)) return false;
	if(!b.pod(r.flags5) || !b.pod(r.flags6) || !b.pod(r.flags7) || !b.pod(r.flags8)) return false;
	if(!b.pod(r.flags9) || !b.pod(r.flags10) || !b.pod(r.flags11)) return false;
	if(!b.pod(r.csensitive)) return false;
	if(!b.pod(r.noreset) || !b.pod(r.nocarry) || !b.pod(r.exstate_reset) || !b.pod(r.exstate_carry)) return false;
	for(int i=0;i<6;i++)
	{
		if(!b.pod(r.layermap[i]) || !b.pod(r.layerscreen[i]) || !b.pod(r.layeropacity[i])) return false;
	}
	if(!b.pod(r.timedwarptics) || !b.pod(r.nextmap) || !b.pod(r.nextscr)) return false;
	for(int i=0;i<128;i++)
	{
		if(!b.pod(r.secretcombo[i]) || !b.pod(r.secretcset[i]) || !b.pod(r.secretflag[i])) return false;
	}
	for(int i=0;i<176;i++)
	{
		if(!b.pod(r.data[i]) || !b.pod(r.sflag[i]) || !b.pod(r.cset[i])) return false;
	}
	if(!b.pod(r.entry_x) || !b.pod(r.entry_y)) return false;
	if(!b.pod(r.oceansfx) || !b.pod(r.bosssfx) || !b.pod(r.secretsfx) || !b.pod(r.holdupsfx)) return false;
	if(!b.pod(r.music)) return false;
	if(!b.pod(r.lens_layer) || !b.pod(r.lens_show) || !b.pod(r.lens_hide)) return false;
	if(!b.pod(r.preloadscript) || !b.pod(r.hidelayers) || !b.pod(r.hidescriptlayers)) return false;
	if(!b.zf(r.screen_gravity) || !b.zf(r.screen_terminal_v)) return false;
	uint32_t n; if(!b.pod(n)) return false;
	if(n > MAXFFCS) { b.ok=false; return false; }
	r.ffcs.resize(n);
	for(uint32_t i=0;i<n;i++) if(!read_ffc(b, r.ffcs[i])) return false;
	return true;
}

void apply_screen(mapscr& s, ScreenRec const& r)
{
	// Identity fields (map/screen/valid) are left as loaded.
	s.guy=r.guy; s.guytile=r.guytile; s.guycs=r.guycs;
	s.roomflags=r.roomflags; s.str=r.str; s.room=r.room;
	s.item=r.item; s.hasitem=r.hasitem;
	for(int i=0;i<4;i++)
	{
		s.tilewarptype[i]=r.tilewarptype[i];
		s.warpreturnx[i]=r.warpreturnx[i]; s.warpreturny[i]=r.warpreturny[i];
		s.door[i]=r.door[i]; s.tilewarpdmap[i]=r.tilewarpdmap[i]; s.tilewarpscr[i]=r.tilewarpscr[i];
		s.sidewarptype[i]=r.sidewarptype[i]; s.path[i]=r.path[i];
		s.sidewarpscr[i]=r.sidewarpscr[i]; s.sidewarpdmap[i]=r.sidewarpdmap[i];
	}
	s.tilewarpoverlayflags=r.tilewarpoverlayflags; s.door_combo_set=r.door_combo_set;
	s.warpreturnc=r.warpreturnc;
	s.stairx=r.stairx; s.stairy=r.stairy; s.itemx=r.itemx; s.itemy=r.itemy;
	s.color=r.color; s.exitdir=r.exitdir;
	for(int i=0;i<10;i++) s.enemy[i]=r.enemy[i];
	s.pattern=r.pattern;
	s.sidewarpoverlayflags=r.sidewarpoverlayflags;
	s.warparrivalx=r.warparrivalx; s.warparrivaly=r.warparrivaly;
	s.maze_transition_wipe=r.maze_transition_wipe; s.sidewarpindex=r.sidewarpindex;
	s.undercombo=r.undercombo; s.undercset=r.undercset; s.catchall=r.catchall;
	s.flags=r.flags; s.flags2=r.flags2; s.flags3=r.flags3; s.flags4=r.flags4;
	s.flags5=r.flags5; s.flags6=r.flags6; s.flags7=r.flags7; s.flags8=r.flags8;
	s.flags9=r.flags9; s.flags10=r.flags10; s.flags11=r.flags11;
	s.csensitive=r.csensitive;
	s.noreset=r.noreset; s.nocarry=r.nocarry; s.exstate_reset=r.exstate_reset; s.exstate_carry=r.exstate_carry;
	for(int i=0;i<6;i++)
	{
		s.layermap[i]=r.layermap[i]; s.layerscreen[i]=r.layerscreen[i]; s.layeropacity[i]=r.layeropacity[i];
	}
	s.timedwarptics=r.timedwarptics; s.nextmap=r.nextmap; s.nextscr=r.nextscr;
	for(int i=0;i<128;i++)
	{
		s.secretcombo[i]=r.secretcombo[i]; s.secretcset[i]=r.secretcset[i]; s.secretflag[i]=r.secretflag[i];
	}
	for(int i=0;i<176;i++)
	{
		s.data[i]=r.data[i]; s.sflag[i]=r.sflag[i]; s.cset[i]=r.cset[i];
	}
	s.entry_x=r.entry_x; s.entry_y=r.entry_y;
	s.oceansfx=r.oceansfx; s.bosssfx=r.bosssfx; s.secretsfx=r.secretsfx; s.holdupsfx=r.holdupsfx;
	s.music=r.music;
	s.lens_layer=r.lens_layer; s.lens_show=r.lens_show; s.lens_hide=r.lens_hide;
	s.preloadscript=r.preloadscript; s.hidelayers=r.hidelayers; s.hidescriptlayers=r.hidescriptlayers;
	s.screen_gravity=r.screen_gravity; s.screen_terminal_v=r.screen_terminal_v;
	if(r.layer==0)
	{
		for(auto const& f : r.ffcs)
		{
			s.ensureFFC(f.index);
			apply_ffc(s.getFFC(f.index), f);
		}
	}
}

// ---------------------------------------------------------------------------
// Transient per-screen state (enemy loading, secrets, doors, side-load)
// ---------------------------------------------------------------------------
struct ScreenStateRec
{
	int32_t screen=0;
	int32_t item_state=0;
	bool loaded_enemies=false,did_enemy_secret=false,triggered_secrets=false;
	int32_t open_doors=0;
	int32_t sl_clk=0,sl_cnt=0,sl_x=0,sl_y=0,sl_pattern=0;
};

// ---------------------------------------------------------------------------
// Hero
// ---------------------------------------------------------------------------
struct HeroRec
{
	SpriteRec s;
	std::vector<std::pair<int16_t,int32_t>> usecounts;
	bool autostep=false,superman=false,inwallm=false,tapping=false,stomping=false,last_hurrah=false,onpassivedmg=false,inair=false;
	int32_t stepnext=0,stepsecret=0;
	int32_t refilling=0,ladderx=0,laddery=0,warpx=0,warpy=0,raftwarpx=0,raftwarpy=0;
	int32_t pushing=0,jumping=0,charging=0,spins=0,fairyclk=0,refillclk=0,drunkclk=0;
	int32_t DivineProtectionShieldClk=0,hoverclk=0,hclk=0,holdclk=0,holditem=0;
	int32_t attackclk=0,attack=0,attackid=0,swordclk=0,itemclk=0,shieldjinxclk=0;
	int32_t didstuff=0,blowcnt=0,stepoutindex=0,stepoutwr=0,stepoutdmap=0,stepoutscreen=0;
	int32_t slashxofs=0,slashyofs=0;
	byte skipstep=0,lstep=0,hopclk=0,whirlwind=0,specialcave=0,hitdir=0,ladderdir=0;
	byte lastdir[4]{};
	byte ladderstart=0,inlikelike=0,damageovertimeclk=0;
	byte switchhookclk=0,switchhookmaxtime=0,switchhookstyle=0,switchhookarg=0;
	int32_t shiftdir=0,lstunclock=0,lbunnyclock=0,sdir=0,sideswimdir=0,diveclk=0,lsave=0,immortal=0;
	int32_t hammer_swim_up_offset=0,hammer_swim_down_offset=0,hammer_swim_left_offset=0,hammer_swim_right_offset=0,swimjump=0;
	int32_t hopdir=0,holddir=0,landswim=0;
	bool ilswim=false,walkable=false;
	int32_t justmoved=0;
	int32_t action=0,tempaction=0;
	int32_t hshandle_id=0,hshead_id=0;
	byte conveyor_flags=0,raftclk=0;
	zfix climb_cover_x,climb_cover_y,respawn_x,respawn_y;
	uint16_t respawn_dmap=0,respawn_scr=0;
	bool forced_respawn_point=false;
	zfix convey_forcex,convey_forcey,falling_oldy;
	byte warp_sound=0;
	bool diagonalMovement=false,bigHitbox=false;
	int32_t steprate=0,tmp_step_boost=0,swimuprate=0,swimsiderate=0,swimdownrate=0;
	zfix shove_offset;
	int8_t itembox_xofs=0,itembox_yofs=0;
	byte itembox_width=0,itembox_height=0;
	byte defence[wMax]{};
	int32_t subscr_speed=0;
	bool is_warping=false;
	byte dying_flags=0;
	int32_t prompt_combo=0,prompt_x=0,prompt_y=0;
	byte prompt_cset=0;
	bool shield_active=false;
	int8_t shield_forcedir=0,conv_forcedir=0;
	int32_t active_shield_id=0;
	word coyotetime=0;
	bool flickerorflash=false,preventsubscreenfalling=false;
	int32_t walkspeed=0;
	int32_t lastHitBy[NUM_HIT_TYPES_USED][2]{};
	int32_t last_lens_id=0,last_grav_boots_id=0,last_rocs_id=0,current_rocs_jump_id=0;
	bool released_jump_button=false;
	int32_t currentscroll=0;
	word last_savepoint_id=0;
	int32_t misc_internal_hero_flags=0,last_cane_of_byrna_item_id=0;
	bool on_sideview_ladder=false;
	zfix standing_on_z;
	bool standing_z_offset=false;
	byte hoverflags=0;
	int32_t extra_jump_count=0;
	int32_t lift_wpn_uid=0,active_wind_uid=0;
	byte liftclk=0,tliftclk=0;
	zfix liftheight;
	uint32_t liftflags=0;
	bool has_last_lift_id=false;
	byte last_lift_id=0;
	zfix ice_vx,ice_vy;
	int32_t ice_combo=0,script_ice_combo=0,sliding=0;
	byte ice_entry_count=0,ice_entry_mcount=0;
	bool ffwarp=false,ffpit=false;
	int32_t cooldowns[MAXITEMS]{};
};

void capture_hero(HeroClass const& h, HeroRec& r)
{
	capture_sprite(h, r.s);
	for(auto const& kv : h.usecounts) r.usecounts.emplace_back(kv.first, kv.second);
	r.autostep=h.autostep; r.superman=h.superman; r.inwallm=h.inwallm; r.tapping=h.tapping;
	r.stomping=h.stomping; r.last_hurrah=h.last_hurrah; r.onpassivedmg=h.onpassivedmg; r.inair=h.inair;
	r.stepnext=int32_t(h.stepnext); r.stepsecret=int32_t(h.stepsecret);
	r.refilling=h.refilling; r.ladderx=h.ladderx; r.laddery=h.laddery;
	r.warpx=h.warpx; r.warpy=h.warpy; r.raftwarpx=h.raftwarpx; r.raftwarpy=h.raftwarpy;
	r.pushing=h.pushing; r.jumping=h.jumping; r.charging=h.charging; r.spins=h.spins;
	r.fairyclk=h.fairyclk; r.refillclk=h.refillclk; r.drunkclk=h.drunkclk;
	r.DivineProtectionShieldClk=h.DivineProtectionShieldClk; r.hoverclk=h.hoverclk;
	r.hclk=h.hclk; r.holdclk=h.holdclk; r.holditem=h.holditem;
	r.attackclk=h.attackclk; r.attack=h.attack; r.attackid=h.attackid;
	r.swordclk=h.swordclk; r.itemclk=h.itemclk; r.shieldjinxclk=h.shieldjinxclk;
	r.didstuff=h.didstuff; r.blowcnt=h.blowcnt;
	r.stepoutindex=h.stepoutindex; r.stepoutwr=h.stepoutwr; r.stepoutdmap=h.stepoutdmap; r.stepoutscreen=h.stepoutscreen;
	r.slashxofs=h.slashxofs; r.slashyofs=h.slashyofs;
	r.skipstep=h.skipstep; r.lstep=h.lstep; r.hopclk=h.hopclk; r.whirlwind=h.whirlwind;
	r.specialcave=h.specialcave; r.hitdir=h.hitdir; r.ladderdir=h.ladderdir;
	for(int i=0;i<4;i++) r.lastdir[i]=h.lastdir[i];
	r.ladderstart=h.ladderstart; r.inlikelike=h.inlikelike; r.damageovertimeclk=h.damageovertimeclk;
	r.switchhookclk=h.switchhookclk; r.switchhookmaxtime=h.switchhookmaxtime;
	r.switchhookstyle=h.switchhookstyle; r.switchhookarg=h.switchhookarg;
	r.shiftdir=h.shiftdir; r.lstunclock=h.lstunclock; r.lbunnyclock=h.lbunnyclock;
	r.sdir=h.sdir; r.sideswimdir=h.sideswimdir; r.diveclk=h.diveclk; r.lsave=h.lsave; r.immortal=h.immortal;
	r.hammer_swim_up_offset=h.hammer_swim_up_offset; r.hammer_swim_down_offset=h.hammer_swim_down_offset;
	r.hammer_swim_left_offset=h.hammer_swim_left_offset; r.hammer_swim_right_offset=h.hammer_swim_right_offset;
	r.swimjump=h.swimjump;
	r.hopdir=h.hopdir; r.holddir=h.holddir; r.landswim=h.landswim;
	r.ilswim=h.ilswim; r.walkable=h.walkable; r.justmoved=h.justmoved;
	r.action=h.action; r.tempaction=h.tempaction;
	r.hshandle_id=h.hshandle_id; r.hshead_id=h.hshead_id;
	r.conveyor_flags=h.conveyor_flags; r.raftclk=h.raftclk;
	r.climb_cover_x=h.climb_cover_x; r.climb_cover_y=h.climb_cover_y;
	r.respawn_x=h.respawn_x; r.respawn_y=h.respawn_y;
	r.respawn_dmap=h.respawn_dmap; r.respawn_scr=h.respawn_scr;
	r.forced_respawn_point=h.forced_respawn_point;
	r.convey_forcex=h.convey_forcex; r.convey_forcey=h.convey_forcey;
	r.falling_oldy=h.falling_oldy;
	r.warp_sound=h.warp_sound;
	r.diagonalMovement=h.diagonalMovement; r.bigHitbox=h.bigHitbox;
	r.steprate=h.steprate; r.tmp_step_boost=h.tmp_step_boost;
	r.swimuprate=h.swimuprate; r.swimsiderate=h.swimsiderate; r.swimdownrate=h.swimdownrate;
	r.shove_offset=h.shove_offset;
	r.itembox_xofs=h.itembox_xofs; r.itembox_yofs=h.itembox_yofs;
	r.itembox_width=h.itembox_width; r.itembox_height=h.itembox_height;
	for(int i=0;i<wMax;i++) r.defence[i]=h.defence[i];
	r.subscr_speed=h.subscr_speed; r.is_warping=h.is_warping; r.dying_flags=h.dying_flags;
	r.prompt_combo=h.prompt_combo; r.prompt_x=h.prompt_x; r.prompt_y=h.prompt_y; r.prompt_cset=h.prompt_cset;
	r.shield_active=h.shield_active; r.shield_forcedir=h.shield_forcedir; r.conv_forcedir=h.conv_forcedir;
	r.active_shield_id=h.active_shield_id; r.coyotetime=h.coyotetime;
	r.flickerorflash=h.flickerorflash; r.preventsubscreenfalling=h.preventsubscreenfalling;
	r.walkspeed=h.walkspeed;
	for(int i=0;i<NUM_HIT_TYPES_USED;i++)
	{
		r.lastHitBy[i][0]=h.lastHitBy[i][0]; r.lastHitBy[i][1]=h.lastHitBy[i][1];
	}
	r.last_lens_id=h.last_lens_id; r.last_grav_boots_id=h.last_grav_boots_id;
	r.last_rocs_id=h.last_rocs_id; r.current_rocs_jump_id=h.current_rocs_jump_id;
	r.released_jump_button=h.released_jump_button;
	r.currentscroll=h.currentscroll; r.last_savepoint_id=h.last_savepoint_id;
	r.misc_internal_hero_flags=h.misc_internal_hero_flags;
	r.last_cane_of_byrna_item_id=h.last_cane_of_byrna_item_id;
	r.on_sideview_ladder=h.on_sideview_ladder;
	r.standing_on_z=h.standing_on_z; r.standing_z_offset=h.standing_z_offset;
	r.hoverflags=h.hoverflags; r.extra_jump_count=h.extra_jump_count;
	r.lift_wpn_uid = h.lift_wpn ? h.lift_wpn->uid : 0;
	r.active_wind_uid = h.active_wind ? h.active_wind->uid : 0;
	r.liftclk=h.liftclk; r.tliftclk=h.tliftclk; r.liftheight=h.liftheight; r.liftflags=h.liftflags;
	r.has_last_lift_id=h.last_lift_id.has_value();
	r.last_lift_id=h.last_lift_id.value_or(0);
	r.ice_vx=h.ice_vx; r.ice_vy=h.ice_vy;
	r.ice_combo=h.ice_combo; r.script_ice_combo=h.script_ice_combo; r.sliding=h.sliding;
	r.ice_entry_count=h.ice_entry_count; r.ice_entry_mcount=h.ice_entry_mcount;
	r.ffwarp=h.ffwarp; r.ffpit=h.ffpit;
	// copy_to is non-const (it may insert zero defaults); harmless on cooldowns.
	const_cast<HeroClass&>(h).item_cooldown.copy_to(r.cooldowns, MAXITEMS);
}

void write_hero(BW& w, HeroRec const& r)
{
	write_sprite(w, r.s);
	w.pod(uint32_t(r.usecounts.size()));
	for(auto const& kv : r.usecounts) { w.pod(kv.first); w.pod(kv.second); }
	w.pod(r.autostep); w.pod(r.superman); w.pod(r.inwallm); w.pod(r.tapping);
	w.pod(r.stomping); w.pod(r.last_hurrah); w.pod(r.onpassivedmg); w.pod(r.inair);
	w.pod(r.stepnext); w.pod(r.stepsecret);
	w.pod(r.refilling); w.pod(r.ladderx); w.pod(r.laddery);
	w.pod(r.warpx); w.pod(r.warpy); w.pod(r.raftwarpx); w.pod(r.raftwarpy);
	w.pod(r.pushing); w.pod(r.jumping); w.pod(r.charging); w.pod(r.spins);
	w.pod(r.fairyclk); w.pod(r.refillclk); w.pod(r.drunkclk);
	w.pod(r.DivineProtectionShieldClk); w.pod(r.hoverclk);
	w.pod(r.hclk); w.pod(r.holdclk); w.pod(r.holditem);
	w.pod(r.attackclk); w.pod(r.attack); w.pod(r.attackid);
	w.pod(r.swordclk); w.pod(r.itemclk); w.pod(r.shieldjinxclk);
	w.pod(r.didstuff); w.pod(r.blowcnt);
	w.pod(r.stepoutindex); w.pod(r.stepoutwr); w.pod(r.stepoutdmap); w.pod(r.stepoutscreen);
	w.pod(r.slashxofs); w.pod(r.slashyofs);
	w.pod(r.skipstep); w.pod(r.lstep); w.pod(r.hopclk); w.pod(r.whirlwind);
	w.pod(r.specialcave); w.pod(r.hitdir); w.pod(r.ladderdir);
	for(int i=0;i<4;i++) w.pod(r.lastdir[i]);
	w.pod(r.ladderstart); w.pod(r.inlikelike); w.pod(r.damageovertimeclk);
	w.pod(r.switchhookclk); w.pod(r.switchhookmaxtime); w.pod(r.switchhookstyle); w.pod(r.switchhookarg);
	w.pod(r.shiftdir); w.pod(r.lstunclock); w.pod(r.lbunnyclock);
	w.pod(r.sdir); w.pod(r.sideswimdir); w.pod(r.diveclk); w.pod(r.lsave); w.pod(r.immortal);
	w.pod(r.hammer_swim_up_offset); w.pod(r.hammer_swim_down_offset);
	w.pod(r.hammer_swim_left_offset); w.pod(r.hammer_swim_right_offset); w.pod(r.swimjump);
	w.pod(r.hopdir); w.pod(r.holddir); w.pod(r.landswim);
	w.pod(r.ilswim); w.pod(r.walkable); w.pod(r.justmoved);
	w.pod(r.action); w.pod(r.tempaction);
	w.pod(r.hshandle_id); w.pod(r.hshead_id);
	w.pod(r.conveyor_flags); w.pod(r.raftclk);
	w.zf(r.climb_cover_x); w.zf(r.climb_cover_y); w.zf(r.respawn_x); w.zf(r.respawn_y);
	w.pod(r.respawn_dmap); w.pod(r.respawn_scr); w.pod(r.forced_respawn_point);
	w.zf(r.convey_forcex); w.zf(r.convey_forcey); w.zf(r.falling_oldy);
	w.pod(r.warp_sound);
	w.pod(r.diagonalMovement); w.pod(r.bigHitbox);
	w.pod(r.steprate); w.pod(r.tmp_step_boost);
	w.pod(r.swimuprate); w.pod(r.swimsiderate); w.pod(r.swimdownrate);
	w.zf(r.shove_offset);
	w.pod(r.itembox_xofs); w.pod(r.itembox_yofs);
	w.pod(r.itembox_width); w.pod(r.itembox_height);
	for(int i=0;i<wMax;i++) w.pod(r.defence[i]);
	w.pod(r.subscr_speed); w.pod(r.is_warping); w.pod(r.dying_flags);
	w.pod(r.prompt_combo); w.pod(r.prompt_x); w.pod(r.prompt_y); w.pod(r.prompt_cset);
	w.pod(r.shield_active); w.pod(r.shield_forcedir); w.pod(r.conv_forcedir);
	w.pod(r.active_shield_id); w.pod(r.coyotetime);
	w.pod(r.flickerorflash); w.pod(r.preventsubscreenfalling);
	w.pod(r.walkspeed);
	for(int i=0;i<NUM_HIT_TYPES_USED;i++) { w.pod(r.lastHitBy[i][0]); w.pod(r.lastHitBy[i][1]); }
	w.pod(r.last_lens_id); w.pod(r.last_grav_boots_id); w.pod(r.last_rocs_id); w.pod(r.current_rocs_jump_id);
	w.pod(r.released_jump_button);
	w.pod(r.currentscroll); w.pod(r.last_savepoint_id);
	w.pod(r.misc_internal_hero_flags); w.pod(r.last_cane_of_byrna_item_id);
	w.pod(r.on_sideview_ladder);
	w.zf(r.standing_on_z); w.pod(r.standing_z_offset);
	w.pod(r.hoverflags); w.pod(r.extra_jump_count);
	w.pod(r.lift_wpn_uid); w.pod(r.active_wind_uid);
	w.pod(r.liftclk); w.pod(r.tliftclk); w.zf(r.liftheight); w.pod(r.liftflags);
	w.pod(r.has_last_lift_id); w.pod(r.last_lift_id);
	w.zf(r.ice_vx); w.zf(r.ice_vy);
	w.pod(r.ice_combo); w.pod(r.script_ice_combo); w.pod(r.sliding);
	w.pod(r.ice_entry_count); w.pod(r.ice_entry_mcount);
	w.pod(r.ffwarp); w.pod(r.ffpit);
	for(int i=0;i<MAXITEMS;i++) w.pod(r.cooldowns[i]);
}

bool read_hero(BR& b, HeroRec& r)
{
	if(!read_sprite(b, r.s)) return false;
	uint32_t n; if(!b.pod(n)) return false;
	if(n > 65536) { b.ok=false; return false; }
	r.usecounts.resize(n);
	for(uint32_t i=0;i<n;i++) if(!b.pod(r.usecounts[i].first) || !b.pod(r.usecounts[i].second)) return false;
	if(!b.pod(r.autostep) || !b.pod(r.superman) || !b.pod(r.inwallm) || !b.pod(r.tapping)) return false;
	if(!b.pod(r.stomping) || !b.pod(r.last_hurrah) || !b.pod(r.onpassivedmg) || !b.pod(r.inair)) return false;
	if(!b.pod(r.stepnext) || !b.pod(r.stepsecret)) return false;
	if(!b.pod(r.refilling) || !b.pod(r.ladderx) || !b.pod(r.laddery)) return false;
	if(!b.pod(r.warpx) || !b.pod(r.warpy) || !b.pod(r.raftwarpx) || !b.pod(r.raftwarpy)) return false;
	if(!b.pod(r.pushing) || !b.pod(r.jumping) || !b.pod(r.charging) || !b.pod(r.spins)) return false;
	if(!b.pod(r.fairyclk) || !b.pod(r.refillclk) || !b.pod(r.drunkclk)) return false;
	if(!b.pod(r.DivineProtectionShieldClk) || !b.pod(r.hoverclk)) return false;
	if(!b.pod(r.hclk) || !b.pod(r.holdclk) || !b.pod(r.holditem)) return false;
	if(!b.pod(r.attackclk) || !b.pod(r.attack) || !b.pod(r.attackid)) return false;
	if(!b.pod(r.swordclk) || !b.pod(r.itemclk) || !b.pod(r.shieldjinxclk)) return false;
	if(!b.pod(r.didstuff) || !b.pod(r.blowcnt)) return false;
	if(!b.pod(r.stepoutindex) || !b.pod(r.stepoutwr) || !b.pod(r.stepoutdmap) || !b.pod(r.stepoutscreen)) return false;
	if(!b.pod(r.slashxofs) || !b.pod(r.slashyofs)) return false;
	if(!b.pod(r.skipstep) || !b.pod(r.lstep) || !b.pod(r.hopclk) || !b.pod(r.whirlwind)) return false;
	if(!b.pod(r.specialcave) || !b.pod(r.hitdir) || !b.pod(r.ladderdir)) return false;
	for(int i=0;i<4;i++) if(!b.pod(r.lastdir[i])) return false;
	if(!b.pod(r.ladderstart) || !b.pod(r.inlikelike) || !b.pod(r.damageovertimeclk)) return false;
	if(!b.pod(r.switchhookclk) || !b.pod(r.switchhookmaxtime) || !b.pod(r.switchhookstyle) || !b.pod(r.switchhookarg)) return false;
	if(!b.pod(r.shiftdir) || !b.pod(r.lstunclock) || !b.pod(r.lbunnyclock)) return false;
	if(!b.pod(r.sdir) || !b.pod(r.sideswimdir) || !b.pod(r.diveclk) || !b.pod(r.lsave) || !b.pod(r.immortal)) return false;
	if(!b.pod(r.hammer_swim_up_offset) || !b.pod(r.hammer_swim_down_offset)) return false;
	if(!b.pod(r.hammer_swim_left_offset) || !b.pod(r.hammer_swim_right_offset) || !b.pod(r.swimjump)) return false;
	if(!b.pod(r.hopdir) || !b.pod(r.holddir) || !b.pod(r.landswim)) return false;
	if(!b.pod(r.ilswim) || !b.pod(r.walkable) || !b.pod(r.justmoved)) return false;
	if(!b.pod(r.action) || !b.pod(r.tempaction)) return false;
	if(!b.pod(r.hshandle_id) || !b.pod(r.hshead_id)) return false;
	if(!b.pod(r.conveyor_flags) || !b.pod(r.raftclk)) return false;
	if(!b.zf(r.climb_cover_x) || !b.zf(r.climb_cover_y) || !b.zf(r.respawn_x) || !b.zf(r.respawn_y)) return false;
	if(!b.pod(r.respawn_dmap) || !b.pod(r.respawn_scr) || !b.pod(r.forced_respawn_point)) return false;
	if(!b.zf(r.convey_forcex) || !b.zf(r.convey_forcey) || !b.zf(r.falling_oldy)) return false;
	if(!b.pod(r.warp_sound)) return false;
	if(!b.pod(r.diagonalMovement) || !b.pod(r.bigHitbox)) return false;
	if(!b.pod(r.steprate) || !b.pod(r.tmp_step_boost)) return false;
	if(!b.pod(r.swimuprate) || !b.pod(r.swimsiderate) || !b.pod(r.swimdownrate)) return false;
	if(!b.zf(r.shove_offset)) return false;
	if(!b.pod(r.itembox_xofs) || !b.pod(r.itembox_yofs)) return false;
	if(!b.pod(r.itembox_width) || !b.pod(r.itembox_height)) return false;
	for(int i=0;i<wMax;i++) if(!b.pod(r.defence[i])) return false;
	if(!b.pod(r.subscr_speed) || !b.pod(r.is_warping) || !b.pod(r.dying_flags)) return false;
	if(!b.pod(r.prompt_combo) || !b.pod(r.prompt_x) || !b.pod(r.prompt_y) || !b.pod(r.prompt_cset)) return false;
	if(!b.pod(r.shield_active) || !b.pod(r.shield_forcedir) || !b.pod(r.conv_forcedir)) return false;
	if(!b.pod(r.active_shield_id) || !b.pod(r.coyotetime)) return false;
	if(!b.pod(r.flickerorflash) || !b.pod(r.preventsubscreenfalling)) return false;
	if(!b.pod(r.walkspeed)) return false;
	for(int i=0;i<NUM_HIT_TYPES_USED;i++)
	{
		if(!b.pod(r.lastHitBy[i][0]) || !b.pod(r.lastHitBy[i][1])) return false;
	}
	if(!b.pod(r.last_lens_id) || !b.pod(r.last_grav_boots_id) || !b.pod(r.last_rocs_id) || !b.pod(r.current_rocs_jump_id)) return false;
	if(!b.pod(r.released_jump_button)) return false;
	if(!b.pod(r.currentscroll) || !b.pod(r.last_savepoint_id)) return false;
	if(!b.pod(r.misc_internal_hero_flags) || !b.pod(r.last_cane_of_byrna_item_id)) return false;
	if(!b.pod(r.on_sideview_ladder)) return false;
	if(!b.zf(r.standing_on_z) || !b.pod(r.standing_z_offset)) return false;
	if(!b.pod(r.hoverflags) || !b.pod(r.extra_jump_count)) return false;
	if(!b.pod(r.lift_wpn_uid) || !b.pod(r.active_wind_uid)) return false;
	if(!b.pod(r.liftclk) || !b.pod(r.tliftclk) || !b.zf(r.liftheight) || !b.pod(r.liftflags)) return false;
	if(!b.pod(r.has_last_lift_id) || !b.pod(r.last_lift_id)) return false;
	if(!b.zf(r.ice_vx) || !b.zf(r.ice_vy)) return false;
	if(!b.pod(r.ice_combo) || !b.pod(r.script_ice_combo) || !b.pod(r.sliding)) return false;
	if(!b.pod(r.ice_entry_count) || !b.pod(r.ice_entry_mcount)) return false;
	if(!b.pod(r.ffwarp) || !b.pod(r.ffpit)) return false;
	for(int i=0;i<MAXITEMS;i++) if(!b.pod(r.cooldowns[i])) return false;
	return true;
}

void apply_hero(HeroClass& h, HeroRec const& r, std::map<int32_t,sprite*> const& uidmap)
{
	apply_sprite(h, r.s);
	h.usecounts.clear();
	for(auto const& kv : r.usecounts) h.usecounts[kv.first]=kv.second;
	h.autostep=r.autostep; h.superman=r.superman; h.inwallm=r.inwallm; h.tapping=r.tapping;
	h.stomping=r.stomping; h.last_hurrah=r.last_hurrah; h.onpassivedmg=r.onpassivedmg; h.inair=r.inair;
	h.stepnext=rpos_t(r.stepnext); h.stepsecret=rpos_t(r.stepsecret);
	h.refilling=r.refilling; h.ladderx=r.ladderx; h.laddery=r.laddery;
	h.warpx=r.warpx; h.warpy=r.warpy; h.raftwarpx=r.raftwarpx; h.raftwarpy=r.raftwarpy;
	h.pushing=r.pushing; h.jumping=r.jumping; h.charging=r.charging; h.spins=r.spins;
	h.fairyclk=r.fairyclk; h.refillclk=r.refillclk; h.drunkclk=r.drunkclk;
	h.DivineProtectionShieldClk=r.DivineProtectionShieldClk; h.hoverclk=r.hoverclk;
	h.hclk=r.hclk; h.holdclk=r.holdclk; h.holditem=r.holditem;
	h.attackclk=r.attackclk; h.attack=r.attack; h.attackid=r.attackid;
	h.swordclk=r.swordclk; h.itemclk=r.itemclk; h.shieldjinxclk=r.shieldjinxclk;
	h.didstuff=r.didstuff; h.blowcnt=r.blowcnt;
	h.stepoutindex=r.stepoutindex; h.stepoutwr=r.stepoutwr; h.stepoutdmap=r.stepoutdmap; h.stepoutscreen=r.stepoutscreen;
	h.slashxofs=r.slashxofs; h.slashyofs=r.slashyofs;
	h.skipstep=r.skipstep; h.lstep=r.lstep; h.hopclk=r.hopclk; h.whirlwind=r.whirlwind;
	h.specialcave=r.specialcave; h.hitdir=r.hitdir; h.ladderdir=r.ladderdir;
	for(int i=0;i<4;i++) h.lastdir[i]=r.lastdir[i];
	h.ladderstart=r.ladderstart; h.inlikelike=r.inlikelike; h.damageovertimeclk=r.damageovertimeclk;
	h.switchhookclk=r.switchhookclk; h.switchhookmaxtime=r.switchhookmaxtime;
	h.switchhookstyle=r.switchhookstyle; h.switchhookarg=r.switchhookarg;
	h.shiftdir=r.shiftdir; h.lstunclock=r.lstunclock; h.lbunnyclock=r.lbunnyclock;
	h.sdir=r.sdir; h.sideswimdir=r.sideswimdir; h.diveclk=r.diveclk; h.lsave=r.lsave; h.immortal=r.immortal;
	h.hammer_swim_up_offset=r.hammer_swim_up_offset; h.hammer_swim_down_offset=r.hammer_swim_down_offset;
	h.hammer_swim_left_offset=r.hammer_swim_left_offset; h.hammer_swim_right_offset=r.hammer_swim_right_offset;
	h.swimjump=r.swimjump;
	h.hopdir=r.hopdir; h.holddir=r.holddir; h.landswim=r.landswim;
	h.ilswim=r.ilswim; h.walkable=r.walkable; h.justmoved=r.justmoved;
	h.action=actiontype(r.action); h.tempaction=actiontype(r.tempaction);
	h.hshandle_id=r.hshandle_id; h.hshead_id=r.hshead_id;
	h.conveyor_flags=r.conveyor_flags; h.raftclk=r.raftclk;
	h.climb_cover_x=r.climb_cover_x; h.climb_cover_y=r.climb_cover_y;
	h.respawn_x=r.respawn_x; h.respawn_y=r.respawn_y;
	h.respawn_dmap=r.respawn_dmap; h.respawn_scr=r.respawn_scr;
	h.forced_respawn_point=r.forced_respawn_point;
	h.convey_forcex=r.convey_forcex; h.convey_forcey=r.convey_forcey;
	h.falling_oldy=r.falling_oldy;
	h.warp_sound=r.warp_sound;
	h.diagonalMovement=r.diagonalMovement; h.bigHitbox=r.bigHitbox;
	h.steprate=r.steprate; h.tmp_step_boost=r.tmp_step_boost;
	h.swimuprate=r.swimuprate; h.swimsiderate=r.swimsiderate; h.swimdownrate=r.swimdownrate;
	h.shove_offset=r.shove_offset;
	h.itembox_xofs=r.itembox_xofs; h.itembox_yofs=r.itembox_yofs;
	h.itembox_width=r.itembox_width; h.itembox_height=r.itembox_height;
	for(int i=0;i<wMax;i++) h.defence[i]=r.defence[i];
	h.subscr_speed=r.subscr_speed; h.is_warping=r.is_warping; h.dying_flags=r.dying_flags;
	h.prompt_combo=r.prompt_combo; h.prompt_x=r.prompt_x; h.prompt_y=r.prompt_y; h.prompt_cset=r.prompt_cset;
	h.shield_active=r.shield_active; h.shield_forcedir=r.shield_forcedir; h.conv_forcedir=r.conv_forcedir;
	h.active_shield_id=r.active_shield_id; h.coyotetime=r.coyotetime;
	h.flickerorflash=r.flickerorflash; h.preventsubscreenfalling=r.preventsubscreenfalling;
	h.walkspeed=r.walkspeed;
	for(int i=0;i<NUM_HIT_TYPES_USED;i++)
	{
		h.lastHitBy[i][0]=r.lastHitBy[i][0]; h.lastHitBy[i][1]=r.lastHitBy[i][1];
	}
	h.last_lens_id=r.last_lens_id; h.last_grav_boots_id=r.last_grav_boots_id;
	h.last_rocs_id=r.last_rocs_id; h.current_rocs_jump_id=r.current_rocs_jump_id;
	h.released_jump_button=r.released_jump_button;
	h.currentscroll=r.currentscroll; h.last_savepoint_id=r.last_savepoint_id;
	h.misc_internal_hero_flags=r.misc_internal_hero_flags;
	h.last_cane_of_byrna_item_id=r.last_cane_of_byrna_item_id;
	h.on_sideview_ladder=r.on_sideview_ladder;
	h.standing_on_z=r.standing_on_z; h.standing_z_offset=r.standing_z_offset;
	h.hoverflags=r.hoverflags; h.extra_jump_count=r.extra_jump_count;
	h.liftclk=r.liftclk; h.tliftclk=r.tliftclk; h.liftheight=r.liftheight; h.liftflags=r.liftflags;
	h.last_lift_id = r.has_last_lift_id ? optional<byte>(r.last_lift_id) : nullopt;
	h.ice_vx=r.ice_vx; h.ice_vy=r.ice_vy;
	h.ice_combo=r.ice_combo; h.script_ice_combo=r.script_ice_combo; h.sliding=r.sliding;
	h.ice_entry_count=r.ice_entry_count; h.ice_entry_mcount=r.ice_entry_mcount;
	h.ffwarp=r.ffwarp; h.ffpit=r.ffpit;
	h.item_cooldown.clear();
	for(int i=0;i<MAXITEMS;i++)
		if(r.cooldowns[i]) h.item_cooldown[i]=r.cooldowns[i];
	// Relink weapon pointers by saved UID; if the weapon is gone, reset the
	// lift/wind state deterministically instead of dangling.
	h.lift_wpn = nullptr;
	h.active_wind = nullptr;
	if(r.lift_wpn_uid)
	{
		auto it = uidmap.find(r.lift_wpn_uid);
		if(it != uidmap.end()) h.lift_wpn = static_cast<weapon*>(it->second);
	}
	if(!h.lift_wpn)
	{
		h.liftclk=0; h.tliftclk=0; h.liftheight=0; h.liftflags=0;
	}
	if(r.active_wind_uid)
	{
		auto it = uidmap.find(r.active_wind_uid);
		if(it != uidmap.end()) h.active_wind = static_cast<weapon*>(it->second);
	}
	// Scripted auto-walk and the FFC platform pointer are re-derived: the walk
	// is cancelled, the platform is recomputed from the restored FFCs.
	h.finish_auto_walk();
	h.clear_platform_ffc();
	h.check_platform_ffc();
	// A state saved mid-screen-scroll resumes with the scroll completed: the
	// hero is already at the saved coordinates on the saved screen.
	if(h.action == scrolling)
	{
		h.action = none;
		h.tempaction = none;
		h.sdir = 0;
	}
	h.update_current_screen();
}

// ---------------------------------------------------------------------------
// Moment globals
// ---------------------------------------------------------------------------
struct GlobalRec
{
	int32_t cur_dmap=0,cur_screen=0,home_screen=0,cur_map=0,dlevel=0;
	int32_t global_frame=0,newscr_clk=0,fadeclk=0,conveyclk=0;
	byte newconveyorclk=0;
	bool freeze_holdup=false,freeze_message=false,freeze_general=false,freeze_ffc=false;
	int32_t wavy=0,quakeclk=0,whistleclk=-1;
	int32_t playing_field_offset=56,original_playing_field_offset=56;
	bool did_secret=false;
};

// ---------------------------------------------------------------------------
// Whole state
// ---------------------------------------------------------------------------
struct QuickState
{
	std::string quest;
	std::vector<char> savblob;
	GlobalRec g;
	std::vector<ScreenRec> screens;
	std::vector<ScreenStateRec> screen_states;
	std::vector<EnemyRec> guys;
	std::vector<ItemRec> items;
	std::vector<WeaponRec> Ewpns, Lwpns, chainlinks;
	std::vector<DecoRec> decorations;
	std::vector<PortalRec> portals;
	MBlockRec mblock;
	HeroRec hero;
};

constexpr char MAGIC[4] = {'Z','C','Q','S'};
constexpr uint32_t STATE_VERSION = 1;

void capture_list_guys(QuickState& st)
{
	for(int32_t i=0; i<guys.Count(); ++i)
	{
		auto* e = dynamic_cast<enemy*>(guys.spr(i));
		if(!e) continue;
		EnemyRec r;
		capture_enemy(e, r);
		st.guys.push_back(std::move(r));
	}
}

template<class T, class Rec, class Cap>
void capture_list(sprite_list& list, std::vector<Rec>& out, Cap cap)
{
	for(int32_t i=0; i<list.Count(); ++i)
	{
		auto* s = dynamic_cast<T*>(list.spr(i));
		if(!s) continue;
		Rec r;
		cap(s, r);
		out.push_back(std::move(r));
	}
}

bool write_state(fs::path const& path, QuickState const& st, std::string& err)
{
	fs::path tmp = path;
	tmp += ".writing";
	FILE* f = fopen(tmp.string().c_str(), "wb");
	if(!f) { err = "cannot open state file for writing"; return false; }
	BW w{f};
	w.raw(MAGIC, 4);
	w.pod(STATE_VERSION);
	w.str(st.quest);
	w.pod(uint64_t(st.savblob.size()));
	w.raw(st.savblob.data(), st.savblob.size());
	// globals
	auto const& g = st.g;
	w.pod(g.cur_dmap); w.pod(g.cur_screen); w.pod(g.home_screen); w.pod(g.cur_map); w.pod(g.dlevel);
	w.pod(g.global_frame); w.pod(g.newscr_clk); w.pod(g.fadeclk); w.pod(g.conveyclk);
	w.pod(g.newconveyorclk);
	w.pod(g.freeze_holdup); w.pod(g.freeze_message); w.pod(g.freeze_general); w.pod(g.freeze_ffc);
	w.pod(g.wavy); w.pod(g.quakeclk); w.pod(g.whistleclk);
	w.pod(g.playing_field_offset); w.pod(g.original_playing_field_offset);
	w.pod(g.did_secret);
	// screens
	w.pod(uint32_t(st.screens.size()));
	for(auto const& s : st.screens) write_screen(w, s);
	// screen states
	w.pod(uint32_t(st.screen_states.size()));
	for(auto const& s : st.screen_states)
	{
		w.pod(s.screen); w.pod(s.item_state);
		w.pod(s.loaded_enemies); w.pod(s.did_enemy_secret); w.pod(s.triggered_secrets);
		w.pod(s.open_doors);
		w.pod(s.sl_clk); w.pod(s.sl_cnt); w.pod(s.sl_x); w.pod(s.sl_y); w.pod(s.sl_pattern);
	}
	// sprite lists
	w.pod(uint32_t(st.guys.size()));
	for(auto const& r : st.guys) write_enemy(w, r);
	w.pod(uint32_t(st.items.size()));
	for(auto const& r : st.items) write_item(w, r);
	w.pod(uint32_t(st.Ewpns.size()));
	for(auto const& r : st.Ewpns) write_weapon(w, r);
	w.pod(uint32_t(st.Lwpns.size()));
	for(auto const& r : st.Lwpns) write_weapon(w, r);
	w.pod(uint32_t(st.chainlinks.size()));
	for(auto const& r : st.chainlinks) write_weapon(w, r);
	w.pod(uint32_t(st.decorations.size()));
	for(auto const& r : st.decorations) write_deco(w, r);
	w.pod(uint32_t(st.portals.size()));
	for(auto const& r : st.portals) write_portal(w, r);
	// moving block + hero
	write_mblock(w, st.mblock);
	write_hero(w, st.hero);
	bool ok = w.ok && !ferror(f);
	if(fclose(f) != 0) ok = false;
	if(!ok)
	{
		fs::remove(tmp);
		err = "state write failed";
		return false;
	}
	std::error_code ec;
	fs::rename(tmp, path, ec);
	if(ec)
	{
		fs::remove(tmp);
		err = "cannot finalize state file: " + ec.message();
		return false;
	}
	return true;
}

bool read_state(fs::path const& path, QuickState& st, std::string& err)
{
	FILE* f = fopen(path.string().c_str(), "rb");
	if(!f) { err = "no quick save yet"; return false; }
	BR b{f};
	char magic[4];
	uint32_t ver;
	if(!b.raw(magic,4) || memcmp(magic,MAGIC,4)!=0 || !b.pod(ver) || ver != STATE_VERSION)
	{
		fclose(f);
		err = "quick state is incompatible (version mismatch)";
		return false;
	}
	if(!b.str(st.quest)) { fclose(f); err="corrupt quick state"; return false; }
	uint64_t blobsz;
	if(!b.pod(blobsz) || blobsz > 64u*1024*1024) { fclose(f); err="corrupt quick state"; return false; }
	st.savblob.resize(size_t(blobsz));
	if(!b.raw(st.savblob.data(), st.savblob.size())) { fclose(f); err="corrupt quick state"; return false; }
	auto& g = st.g;
	if(!b.pod(g.cur_dmap) || !b.pod(g.cur_screen) || !b.pod(g.home_screen) || !b.pod(g.cur_map) || !b.pod(g.dlevel)) goto bad;
	if(!b.pod(g.global_frame) || !b.pod(g.newscr_clk) || !b.pod(g.fadeclk) || !b.pod(g.conveyclk)) goto bad;
	if(!b.pod(g.newconveyorclk)) goto bad;
	if(!b.pod(g.freeze_holdup) || !b.pod(g.freeze_message) || !b.pod(g.freeze_general) || !b.pod(g.freeze_ffc)) goto bad;
	if(!b.pod(g.wavy) || !b.pod(g.quakeclk) || !b.pod(g.whistleclk)) goto bad;
	if(!b.pod(g.playing_field_offset) || !b.pod(g.original_playing_field_offset)) goto bad;
	if(!b.pod(g.did_secret)) goto bad;
	{
		uint32_t n;
		if(!b.pod(n) || n > 4096) goto bad;
		st.screens.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_screen(b, st.screens[i])) goto bad;
		if(!b.pod(n) || n > 65536) goto bad;
		st.screen_states.resize(n);
		for(uint32_t i=0;i<n;i++)
		{
			auto& s = st.screen_states[i];
			if(!b.pod(s.screen) || !b.pod(s.item_state)) goto bad;
			if(!b.pod(s.loaded_enemies) || !b.pod(s.did_enemy_secret) || !b.pod(s.triggered_secrets)) goto bad;
			if(!b.pod(s.open_doors)) goto bad;
			if(!b.pod(s.sl_clk) || !b.pod(s.sl_cnt) || !b.pod(s.sl_x) || !b.pod(s.sl_y) || !b.pod(s.sl_pattern)) goto bad;
		}
		if(!b.pod(n) || n > SLMAX) goto bad;
		st.guys.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_enemy(b, st.guys[i])) goto bad;
		if(!b.pod(n) || n > SLMAX) goto bad;
		st.items.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_item(b, st.items[i])) goto bad;
		if(!b.pod(n) || n > SLMAX) goto bad;
		st.Ewpns.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_weapon(b, st.Ewpns[i])) goto bad;
		if(!b.pod(n) || n > SLMAX) goto bad;
		st.Lwpns.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_weapon(b, st.Lwpns[i])) goto bad;
		if(!b.pod(n) || n > SLMAX) goto bad;
		st.chainlinks.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_weapon(b, st.chainlinks[i])) goto bad;
		if(!b.pod(n) || n > SLMAX) goto bad;
		st.decorations.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_deco(b, st.decorations[i])) goto bad;
		if(!b.pod(n) || n > SLMAX) goto bad;
		st.portals.resize(n);
		for(uint32_t i=0;i<n;i++) if(!read_portal(b, st.portals[i])) goto bad;
	}
	if(!read_mblock(b, st.mblock)) goto bad;
	if(!read_hero(b, st.hero)) goto bad;
	fclose(f);
	return true;
bad:
	fclose(f);
	err = "corrupt quick state";
	return false;
}

} // anonymous namespace

std::string savestate_quick_path()
{
	return state_path().string();
}

bool savestate_has_quick()
{
	std::error_code ec;
	return fs::exists(state_path(), ec);
}

bool savestate_quick_save(std::string& err)
{
	if(!game || !Playing)
	{
		err = "no active game";
		return false;
	}
	// Record the current screen as the continue point (same as save_game(true)),
	// so the embedded progress file also resumes here on a normal Continue.
	game->set_continue_scrn(Hero.current_screen >= 0x80 ? home_screen : Hero.current_screen);
	game->set_continue_dmap(cur_dmap);

	QuickState st;
	st.quest = quest_key();

	// 1) gamedata progress via the engine's own save writer.
	fs::create_directories(state_path().parent_path());
	fs::path tmpsav = state_path();
	tmpsav += ".savtmp";
	{
		save_t s;
		s.path = tmpsav;
		s.write_to_disk = true;
		s.game = game;
		s.header = &game->header;
		s.index = 0;
		bool ok = _write_save(&s, err, true);
		// save_t deletes whatever it holds: detach the LIVE game before dtor.
		s.game = nullptr;
		s.header = nullptr;
		if(!ok) return false;
	}
	{
		FILE* f = fopen(tmpsav.string().c_str(), "rb");
		if(!f) { err = "cannot read back progress blob"; return false; }
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if(sz <= 0) { fclose(f); fs::remove(tmpsav); err="empty progress blob"; return false; }
		st.savblob.resize(size_t(sz));
		bool ok = fread(st.savblob.data(), 1, st.savblob.size(), f) == st.savblob.size();
		fclose(f);
		fs::remove(tmpsav);
		if(!ok) { err = "cannot read back progress blob"; return false; }
	}

	// 2) moment globals
	auto& g = st.g;
	g.cur_dmap=cur_dmap; g.cur_screen=cur_screen; g.home_screen=home_screen;
	g.cur_map=cur_map; g.dlevel=dlevel;
	g.global_frame=global_frame; g.newscr_clk=newscr_clk; g.fadeclk=fadeclk; g.conveyclk=conveyclk;
	g.newconveyorclk=newconveyorclk;
	g.freeze_holdup=freeze_holdup; g.freeze_message=freeze_message;
	g.freeze_general=freeze_general; g.freeze_ffc=freeze_ffc;
	g.wavy=wavy; g.quakeclk=quakeclk; g.whistleclk=whistleclk;
	g.playing_field_offset=playing_field_offset;
	g.original_playing_field_offset=original_playing_field_offset;
	// (hero.h's `did_secret` extern has no definition in the engine; not used.)
	g.did_secret=false;

	// 3) screens: every screen of the current scrolling region (or just the
	// current screen), base + layers, with combo buffers and FFCs.
	std::vector<int32_t> screens;
	if(is_in_scrolling_region())
	{
		for(int32_t s=0; s<128; ++s)
			if(is_in_current_region(s)) screens.push_back(s);
	}
	else
	{
		screens.push_back(cur_screen);
		if(cur_screen >= 0x80 && home_screen != cur_screen)
			screens.push_back(home_screen);
	}
	for(int32_t scr : screens)
	{
		for(int32_t layer=0; layer<7; ++layer)
		{
			mapscr* ms = get_scr_layer(scr, layer);
			if(!ms) continue;
			ScreenRec r;
			capture_screen(*ms, scr, layer, r);
			st.screens.push_back(std::move(r));
		}
	}

	// 4) transient screen state
	for(auto const& kv : get_screen_states())
	{
		ScreenStateRec r;
		r.screen = kv.first;
		r.item_state = int32_t(kv.second.item_state);
		r.loaded_enemies = kv.second.loaded_enemies;
		r.did_enemy_secret = kv.second.did_enemy_secret;
		r.triggered_secrets = kv.second.triggered_secrets;
		r.open_doors = kv.second.open_doors;
		r.sl_clk = kv.second.side_load.clk; r.sl_cnt = kv.second.side_load.cnt;
		r.sl_x = kv.second.side_load.x; r.sl_y = kv.second.side_load.y;
		r.sl_pattern = kv.second.side_load.pattern;
		st.screen_states.push_back(r);
	}

	// 5) sprite lists
	capture_list_guys(st);
	capture_list<item>(items, st.items, capture_item);
	capture_list<weapon>(Ewpns, st.Ewpns, capture_weapon);
	capture_list<weapon>(Lwpns, st.Lwpns, capture_weapon);
	capture_list<weapon>(chainlinks, st.chainlinks, capture_weapon);
	capture_list<decoration>(decorations, st.decorations, capture_deco);
	capture_list<portal>(portals, st.portals, capture_portal);

	// 6) moving block + hero
	capture_mblock(mblock2, st.mblock);
	capture_hero(Hero, st.hero);

	return write_state(state_path(), st, err);
}

bool savestate_quick_load(std::string& err)
{
	if(!game)
	{
		err = "no active game";
		return false;
	}
	QuickState st;
	if(!read_state(state_path(), st, err)) return false;
	if(st.quest != quest_key())
	{
		err = "quick state belongs to a different quest";
		return false;
	}
	if(st.g.cur_dmap < 0 || st.g.cur_dmap >= MAXDMAPS)
	{
		err = "quick state has invalid dmap";
		return false;
	}

	// --- Restore progress (gamedata) from the embedded save blob. ---
	fs::path tmpsav = state_path();
	tmpsav += ".savtmp";
	{
		FILE* f = fopen(tmpsav.string().c_str(), "wb");
		if(!f) { err = "cannot stage progress blob"; return false; }
		bool ok = fwrite(st.savblob.data(), 1, st.savblob.size(), f) == st.savblob.size();
		fclose(f);
		if(!ok) { fs::remove(tmpsav); err="cannot stage progress blob"; return false; }
	}
	save_t loaded{};
	{
		std::string e;
		bool ok = _load_from_save_file_expect_one(ReadMode::All, tmpsav, loaded, e);
		fs::remove(tmpsav);
		if(!ok || !loaded.game) { err = "quick state progress unreadable: " + e; return false; }
	}
	game->Copy(*loaded.game);

	// --- Re-enter the saved screen through the engine's continue path. This
	// handles cross-dmap travel (init_dmap, palettes, dmap scripts) and loads
	// the screen + layers fresh. ---
	game->set_continue_scrn(st.g.cur_screen >= 0x80 ? st.g.home_screen : st.g.cur_screen);
	game->set_continue_dmap(st.g.cur_dmap);
	lastentrance_dmap = st.g.cur_dmap;
	lastentrance = st.g.cur_screen;
	cont_game();
	// cont_game() rewrites a few progress fields (continue-hearts life, the
	// level-0 boss-killed bit). Re-copy the snapshot so progress is EXACTLY as
	// saved, not as a fresh continue would make it.
	game->Copy(*loaded.game);

	// --- Overwrite the freshly loaded screens with the saved combo buffers,
	// flags and FFCs. ---
	for(auto const& r : st.screens)
	{
		mapscr* ms = get_scr_layer(r.screen, r.layer);
		if(!ms) continue;
		apply_screen(*ms, r);
	}
	for(auto const& r : st.screen_states)
	{
		auto& ss = get_screen_state(r.screen);
		ss.item_state = ScreenItemState(r.item_state);
		ss.loaded_enemies = r.loaded_enemies;
		ss.did_enemy_secret = r.did_enemy_secret;
		ss.triggered_secrets = r.triggered_secrets;
		ss.open_doors = r.open_doors;
		ss.side_load.clk = r.sl_clk; ss.side_load.cnt = r.sl_cnt;
		ss.side_load.x = r.sl_x; ss.side_load.y = r.sl_y;
		ss.side_load.pattern = r.sl_pattern;
	}

	// --- Clear the respawned world and rebuild the saved sprites. ---
	// Drop hero->weapon references first so list deletion cannot dangle them.
	Hero.lift_wpn = nullptr;
	Hero.active_wind = nullptr;
	// Script engine data for sprite/screen/FFC scripts is reset: the scripts
	// restart deterministically from the restored script configs next frame.
	FFCore.clear_script_engine_data_of_type(ScriptType::NPC);
	FFCore.clear_script_engine_data_of_type(ScriptType::ItemSprite);
	FFCore.clear_script_engine_data_of_type(ScriptType::Lwpn);
	FFCore.clear_script_engine_data_of_type(ScriptType::Ewpn);
	FFCore.clear_script_engine_data_of_type(ScriptType::FFC);
	FFCore.clear_script_engine_data_of_type(ScriptType::Screen);
	guys.clear(true);
	items.clear(true);
	Ewpns.clear(true);
	Lwpns.clear(true);
	chainlinks.clear(true);
	decorations.clear(true);
	portals.clear(true);

	std::map<int32_t,sprite*> uidmap;
	std::map<int32_t,enemy*> enemymap;
	auto register_sprite = [&](sprite* s, int32_t uid)
	{
		if(!s) return;
		if(uid) s->reassignUid(uid);
		uidmap[s->uid] = s;
	};
	// Hero keeps its saved UID (1) so weapons parented to Hero relink.
	if(st.hero.s.uid) Hero.reassignUid(st.hero.s.uid);
	uidmap[Hero.uid] = &Hero;

	// Enemies first: weapon constructors look up their parent enemy by UID.
	for(auto const& r : st.guys)
	{
		enemy* e = build_enemy(r, uidmap);
		if(!e)
		{
			al_trace("savestate: skipped enemy tag %d (missing parent)\n", r.tag);
			continue;
		}
		apply_enemy(e, r);
		if(!guys.add(e)) { delete e; continue; }
		register_sprite(e, r.s.uid);
		enemymap[e->uid] = e;
	}
	for(auto const& r : st.items)
	{
		item* i = new item(r.s.x, r.s.y, r.s.z, r.s.id, r.ctor_pickup, r.ctor_clk, r.ctor_dummy);
		apply_item(i, r);
		if(!items.add(i)) { delete i; continue; }
		register_sprite(i, r.s.uid);
	}
	auto build_weapons = [&](std::vector<WeaponRec> const& recs, sprite_list& list)
	{
		for(auto const& r : recs)
		{
			weapon* w = build_weapon(r);
			apply_weapon(w, r);
			if(!list.add(w)) { delete w; continue; }
			register_sprite(w, r.s.uid);
		}
	};
	build_weapons(st.Ewpns, Ewpns);
	build_weapons(st.Lwpns, Lwpns);
	build_weapons(st.chainlinks, chainlinks);
	for(auto const& r : st.decorations)
	{
		decoration* d = build_deco(r);
		apply_deco(d, r);
		if(!decorations.add(d)) { delete d; continue; }
		register_sprite(d, r.s.uid);
	}
	for(auto const& r : st.portals)
	{
		portal* p = new portal();
		apply_portal(p, r);
		if(!portals.add(p)) { delete p; continue; }
		register_sprite(p, r.s.uid);
	}

	// --- Relink parent/child + summoner relationships by saved UID. ---
	for(auto const& r : st.guys)
	{
		auto it = uidmap.find(r.s.uid);
		if(it == uidmap.end()) continue;
		sprite* s = it->second;
		if(r.s.parent_uid)
		{
			auto pit = uidmap.find(r.s.parent_uid);
			if(pit != uidmap.end()) s->setParent(pit->second);
		}
		enemy* e = dynamic_cast<enemy*>(s);
		if(!e) continue;
		if(r.summoner_parent_uid)
		{
			auto pit = enemymap.find(r.summoner_parent_uid);
			if(pit != enemymap.end()) e->set_summoner_parent(pit->second);
		}
	}
	auto relink_weapon_parents = [&](std::vector<WeaponRec> const& recs)
	{
		for(auto const& r : recs)
		{
			if(!r.s.parent_uid) continue;
			auto it = uidmap.find(r.s.uid);
			auto pit = uidmap.find(r.s.parent_uid);
			if(it != uidmap.end() && pit != uidmap.end())
				it->second->setParent(pit->second);
		}
	};
	relink_weapon_parents(st.Ewpns);
	relink_weapon_parents(st.Lwpns);
	relink_weapon_parents(st.chainlinks);
	for(auto const& r : st.decorations)
	{
		if(!r.s.parent_uid) continue;
		auto it = uidmap.find(r.s.uid);
		auto pit = uidmap.find(r.s.parent_uid);
		if(it != uidmap.end() && pit != uidmap.end())
			it->second->setParent(pit->second);
	}
	for(auto const& r : st.portals)
	{
		if(!r.s.parent_uid) continue;
		auto it = uidmap.find(r.s.uid);
		auto pit = uidmap.find(r.s.parent_uid);
		if(it != uidmap.end() && pit != uidmap.end())
			it->second->setParent(pit->second);
	}

	// --- Moving block, hero, globals. ---
	apply_mblock(mblock2, st.mblock);
	apply_hero(Hero, st.hero, uidmap);

	auto const& g = st.g;
	cur_dmap=g.cur_dmap; cur_screen=g.cur_screen; home_screen=g.home_screen;
	cur_map=g.cur_map; dlevel=g.dlevel;
	global_frame=g.global_frame; newscr_clk=g.newscr_clk; fadeclk=g.fadeclk; conveyclk=g.conveyclk;
	newconveyorclk=g.newconveyorclk;
	// Freeze flags tied to transient script/dialogue state cannot survive the
	// script re-init below: a restored freeze_message with no live message box
	// (or freeze_general with no running cutscene) would soft-lock the game.
	// Re-init them deterministically; scripts re-assert their own freezes.
	// The item hold-up freeze IS restorable: it is driven by Hero.holdclk.
	freeze_holdup = g.freeze_holdup && Hero.holdclk > 0;
	freeze_message = false;
	freeze_general = false;
	freeze_ffc = false;
	// A mid-scroll save resumes with the scroll completed (see apply_hero).
	if(st.hero.action == scrolling)
	{
		freeze_general = false;
		wavy = 0; quakeclk = 0;
	}
	else
	{
		wavy=g.wavy; quakeclk=g.quakeclk;
	}
	whistleclk=g.whistleclk;
	playing_field_offset=g.playing_field_offset;
	original_playing_field_offset=g.original_playing_field_offset;

	// Re-derive combo-position/slope caches for the overwritten combo buffer,
	// and recenter the viewport on the restored hero.
	update_slope_comboposes();
	cpos_force_update();
	update_viewport();
	update_subscreens();
	return true;
}
