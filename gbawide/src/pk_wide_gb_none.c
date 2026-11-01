// PokeDaisy's EXPAND for Game Boy games (its pk_wide_gb.c) is left out: only GBA games play here,
// so pk_wide.c's calls into it find no Game Boy game.
#include "../third_party/pokedaisy/pk_wide.h"

int pk_wide_gb_knows(uint32_t crc) { (void)crc; return 0; }
int pk_wide_gb_configure(uint32_t crc) { (void)crc; return 0; }
void pk_wide_gb_reset(void) {}
int pk_wide_gb_view(struct mCore* core, const uint32_t* frame, uint32_t* out, struct PkWideView view, uint32_t* centre)
{
    (void)core; (void)frame; (void)out; (void)view; (void)centre;
    return 0;
}
int pk_wide_gb_scene(void) { return PK_WIDE_SCENE_OFF; }
int pk_wide_gb_wants_close(void) { return 0; }
int pk_wide_gb_last_mode(void) { return 0; }
int pk_wide_gb_last_score(void) { return 0; }
int pk_wide_gb_last_torn(void) { return 0; }
