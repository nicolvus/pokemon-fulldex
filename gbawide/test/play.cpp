// gbawide-play <rom.gba> <saves folder>: plays one GBA game as the game list would (for tests:
// GBAW_TEST_* in src/gbawide.cpp).
#include <cstdio>

#include "embed.h"

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: gbawide-play <rom.gba> <saves folder>\n");
        return 1;
    }
    GbaWide::Options o;
    o.saves = argv[2];
    o.data = argv[2];
    GbaWide::Play(argv[1], o);
    return 0;
}
