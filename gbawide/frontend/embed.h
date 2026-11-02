#pragma once
// The GBA games of the all-games program (frontend/gba_games.h): finds them in a folder and plays
// one, with mGBA and PokeDaisy's EXPAND (the overworld drawn past the GBA's edges, every language
// and revision of Emerald, FireRed, LeafGreen, Ruby and Sapphire: third_party/pokedaisy).
#include <cstddef>
#include <string>
#include <vector>

// (No RetroAchievements here: frontend/gba_ra.h plays the game alone.)
#define GBAWIDE_NO_ACHIEVEMENTS 1

namespace GbaWide
{

// A GBA game in a folder.
struct ListGame
{
    std::string file;    // the ROM's path
    std::string key;     // its name for saves (<key>.sav): emerald, firered, leafgreen; else from the file's name
    std::string name;    // as the list shows it (POKEMON EMERALD)
    std::string art;     // built-in box art: none
    bool hasSave = false;
};

// The .gba files in `roms`, their saves looked for in `saves`.
std::vector<ListGame> Find(const std::string& roms, const std::string& saves);

// Box art built in for `art`: none in this build.
bool Art(const std::string& art, const unsigned char*& jpeg, size_t& size);

struct Options
{
    std::string roms;       // the ROM's folder
    std::string saves;      // where <key>.sav goes
    std::string data;       // the program's folder (gba-settings.ini: scaling, volume)
    bool fullscreen = false;
    int w = 0, h = 0;       // the window's size (0: 1280 x 720)
};

// Plays `rom` until the player chooses QUIT in its pause menu (Select and Start together, ZL and ZR
// together, the right stick pressed, Guide or Escape) or closes the window; starts and stops SDL.
void Play(const std::string& rom, const Options& options);

}
