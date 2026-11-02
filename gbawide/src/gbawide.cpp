// The GBA games (frontend/embed.h): mGBA runs the game; PokeDaisy's EXPAND (third_party/pokedaisy,
// GPL-3.0) draws the overworld past the GBA's 240 pixels from the game's memory, so the picture fills
// a wide screen. The host loop follows PokeDaisy's Linux host (linux/main.c): SDL2 window, audio
// queue at 32768 Hz, controller and keyboard.
#include "../frontend/embed.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#include <mgba-util/vfs.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>

#include "ui.h"

extern "C" {
#include "../third_party/pokedaisy/pk_wide.h"
}

namespace GbaWide
{
namespace
{

// GBA keys (mGBA's bit order).
enum { K_A, K_B, K_SELECT, K_START, K_RIGHT, K_LEFT, K_UP, K_DOWN, K_R, K_L };

bool Exists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool EndsWithGba(const std::string& name)
{
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".gba";
}

// The English versions' names and save keys (as Pokemon GBA Widescreen has them, so saves carry
// on); any other ROM is named after its file (pokemon-esmeralda, POKEMON ESMERALDA).
void Describe(const std::string& path, ListGame& g)
{
    char code[5] = {};
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        if (fseek(f, 0xAC, SEEK_SET) != 0 || fread(code, 1, 4, f) != 4) code[0] = 0;
        fclose(f);
    }
    static const struct { const char* code; const char* key; const char* name; } english[] = {
        { "BPEE", "emerald", "POKEMON EMERALD" },
        { "BPRE", "firered", "POKEMON FIRERED" },
        { "BPGE", "leafgreen", "POKEMON LEAFGREEN" },
    };
    for (const auto& e : english)
        if (!strcmp(code, e.code))
        {
            g.key = e.key;
            g.name = e.name;
            return;
        }
    std::string base = path.substr(path.find_last_of("/\\") + 1);
    base = base.substr(0, base.size() - 4);
    g.name.clear();
    g.key.clear();
    for (char c : base)
    {
        g.name += (char)toupper((unsigned char)c);
        const char k = (char)tolower((unsigned char)c);
        if (isalnum((unsigned char)k)) g.key += k;
        else if (!g.key.empty() && g.key.back() != '-') g.key += '-';
    }
    while (!g.key.empty() && g.key.back() == '-') g.key.pop_back();
    if (g.key.empty()) g.key = "game";
}

// Settings shared with the DS games (Launcher::SyncGbaSettings writes them): scaling, volume.
std::string Setting(const std::string& ini, const std::string& key)
{
    std::ifstream f(ini);
    std::string line;
    while (std::getline(f, line))
        if (!line.compare(0, key.size() + 1, key + "=")) return line.substr(key.size() + 1);
    return "";
}

// Test runs (headless): GBAW_TEST_FRAMES frames, GBAW_TEST_INPUT a script ("first last KEY ...",
// KEY one of A B SELECT START RIGHT LEFT UP DOWN R L), GBAW_TEST_SHOTS frames whose picture goes to
// GBAW_TEST_DIR/gba_<frame>.bmp, GBAW_TEST_STATE_SAVE/_AT and GBAW_TEST_STATE_LOAD a save state.
struct TestEvent { int first, last; unsigned keys; };
std::vector<TestEvent> LoadScript(const char* path)
{
    std::vector<TestEvent> out;
    if (!path) return out;
    std::ifstream f(path);
    std::string line;
    static const char* names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
    while (std::getline(f, line))
    {
        std::istringstream ss(line);
        TestEvent e{};
        std::string k;
        if (!(ss >> e.first >> e.last)) continue;
        while (ss >> k)
            for (int i = 0; i < 10; i++)
                if (k == names[i]) e.keys |= 1u << i;
        out.push_back(e);
    }
    return out;
}

uint32_t video[256 * 256];
uint32_t wideSource[PK_WIDE_W * PK_WIDE_H];
uint32_t wideFrame[PK_WIDE_MAX_VIEW_W * PK_WIDE_MAX_VIEW_H];
SDL_AudioDeviceID audio = 0;
float volume = 1.0f;

void PostAudio(struct mAVStream*, blip_t* left, blip_t* right)
{
    int16_t out[2048];
    int n = blip_samples_avail(left);
    if (n > 1024) n = 1024;
    blip_read_samples(left, out, n, true);
    blip_read_samples(right, out + 1, n, true);
    if (volume < 1.0f)
        for (int i = 0; i < n * 2; i++) out[i] = (int16_t)(out[i] * volume);
    // (The game is paced by this queue: Play runs a frame when it is nearly empty.)
    if (audio && SDL_GetQueuedAudioSize(audio) < 32768) SDL_QueueAudio(audio, out, n * 4);
}

void QuietLog(struct mLogger*, int, enum mLogLevel, const char*, va_list) {}
struct mLogger quietLogger = { QuietLog, nullptr };

// The EXPAND window for an ow x oh screen: the screen's size over the game's whole scale, within
// pk_wide's largest view, never narrower than the game, with even margins (PokeDaisy's wide_window).
void WideWindow(int ow, int oh, unsigned fw, unsigned fh, unsigned& w, unsigned& h)
{
    int scale = std::max(1, std::min(ow / (int)fw, oh / (int)fh));
    unsigned x = (unsigned)ow / scale, y = (unsigned)oh / scale;
    x = std::min<unsigned>(PK_WIDE_MAX_VIEW_W, std::max(fw, x));
    y = std::min<unsigned>(PK_WIDE_MAX_VIEW_H, std::max(fh, y));
    w = x - ((x - fw) & 1);
    h = y - ((y - fh) & 1);
}

int KeyOfButton(int b)
{
    // (SDL's buttons by position, as on an Xbox pad: the Switch's A, on the right, is SDL's B.)
    switch (b)
    {
    case SDL_CONTROLLER_BUTTON_B: return K_A;
    case SDL_CONTROLLER_BUTTON_A: return K_B;
    case SDL_CONTROLLER_BUTTON_BACK: return K_SELECT;
    case SDL_CONTROLLER_BUTTON_START: return K_START;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHT;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_L;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_R;
    default: return -1;
    }
}

int KeyOfKey(SDL_Keycode k)
{
    switch (k)
    {
    case SDLK_x: return K_A;
    case SDLK_z: return K_B;
    case SDLK_BACKSPACE: case SDLK_RSHIFT: return K_SELECT;
    case SDLK_RETURN: return K_START;
    case SDLK_UP: return K_UP;
    case SDLK_DOWN: return K_DOWN;
    case SDLK_LEFT: return K_LEFT;
    case SDLK_RIGHT: return K_RIGHT;
    case SDLK_a: case SDLK_q: return K_L;
    case SDLK_s: case SDLK_w: return K_R;
    default: return -1;
    }
}

void SaveBmp(const std::string& path, const uint32_t* px, int w, int h, int stride)
{
#ifdef __SWITCH__
    (void)path; (void)px; (void)w; (void)h; (void)stride; // (test pictures are taken on a PC)
#else
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom((void*)px, w, h, 32, stride * 4, SDL_PIXELFORMAT_XBGR8888);
    if (s)
    {
        SDL_SaveBMP(s, path.c_str());
        SDL_FreeSurface(s);
    }
#endif
}

}

std::vector<ListGame> Find(const std::string& roms, const std::string& saves)
{
    std::vector<ListGame> out;
    DIR* d = opendir(roms.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name[0] == '.' || !EndsWithGba(name)) continue;
        ListGame g;
        g.file = roms + "/" + name;
        Describe(g.file, g);
        g.hasSave = Exists(saves + "/" + g.key + ".sav");
        out.push_back(std::move(g));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const ListGame& a, const ListGame& b) { return a.name < b.name; });
    return out;
}

bool Art(const std::string&, const unsigned char*&, size_t&) { return false; }

void Play(const std::string& rom, const Options& o)
{
    mLogSetDefaultLogger(&quietLogger);
    ListGame g;
    Describe(rom, g);
    const char* testFrames = getenv("GBAW_TEST_FRAMES");
    const int maxFrames = testFrames ? atoi(testFrames) : 0;
    const std::vector<TestEvent> script = LoadScript(getenv("GBAW_TEST_INPUT"));
    std::vector<int> shots;
    if (const char* s = getenv("GBAW_TEST_SHOTS"))
        for (std::istringstream ss(s); ss.good();)
        {
            int f;
            char comma;
            if (ss >> f) shots.push_back(f);
            ss >> comma;
        }
    const std::string testDir = getenv("GBAW_TEST_DIR") ? getenv("GBAW_TEST_DIR") : ".";

    // (Sound is not needed: without it the game keeps time by the clock.)
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "[gba] no sound: %s\n", SDL_GetError());
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr, "[gba] SDL: %s\n", SDL_GetError());
        return;
    }
    const std::string ini = o.data + "/gba-settings.ini";
    const std::string vol = Setting(ini, "volume");
    volume = vol.empty() ? 1.0f : std::clamp(atoi(vol.c_str()), 0, 100) / 100.0f;
    const int ww = o.w > 0 ? o.w : 1280, wh = o.h > 0 ? o.h : 720;
    SDL_Window* window = SDL_CreateWindow("Pokémon Fulldex", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                                          SDL_WINDOW_RESIZABLE | (o.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, maxFrames ? 0 : SDL_RENDERER_PRESENTVSYNC) : nullptr;
    SDL_Texture* gameTex = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, 240, 160) : nullptr;
    SDL_Texture* wideTex = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING,
                                                        PK_WIDE_MAX_VIEW_W, PK_WIDE_MAX_VIEW_H) : nullptr;
    if (!renderer || !gameTex || !wideTex)
    {
        fprintf(stderr, "[gba] window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }
    SDL_AudioSpec want{};
    want.freq = 32768;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    std::vector<SDL_GameController*> pads;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            if (SDL_GameController* p = SDL_GameControllerOpen(i)) pads.push_back(p);

    struct VFile* vf = VFileOpen(rom.c_str(), O_RDONLY);
    struct mCore* core = vf ? mCoreFindVF(vf) : nullptr;
    bool ok = core && core->init(core);
    if (ok)
    {
        mCoreInitConfig(core, nullptr);
        core->setVideoBuffer(core, (color_t*)video, 256);
        core->setAudioBufferSize(core, 512);
        ok = core->loadROM(core, vf);
    }
    if (!ok)
    {
        fprintf(stderr, "[gba] %s: not a ROM mGBA can play\n", rom.c_str());
        if (core) core->deinit(core);
        else if (vf) vf->close(vf);
    }
    else
    {
        mkdir(o.saves.c_str(), 0755);
        const std::string savePath = o.saves + "/" + g.key + ".sav";
        // (The save as it was, before this game writes to it.)
        if (Exists(savePath) && !maxFrames)
        {
            std::ifstream src(savePath, std::ios::binary);
            std::ofstream dst(savePath + ".bak", std::ios::binary);
            dst << src.rdbuf();
        }
        if (struct VFile* sv = VFileOpen(savePath.c_str(), O_CREAT | O_RDWR)) core->loadSave(core, sv);
        struct mAVStream stream{};
        stream.postAudioBuffer = PostAudio;
        core->setAVStream(core, &stream);
        for (int i = 0; i < 2; i++) blip_set_rates(core->getAudioChannel(core, i), core->frequency(core), 32768);
        core->reset(core);
        if (const char* st = getenv("GBAW_TEST_STATE_LOAD"))
            if (struct VFile* sf = VFileOpen(st, O_RDONLY))
            {
                mCoreLoadStateNamed(core, sf, 0);
                sf->close(sf);
            }
        uint32_t crc = 0;
        core->checksum(core, &crc, mCHECKSUM_CRC32);
        const bool wide = pk_wide_configure(crc) != 0;
        fprintf(stderr, "[gba] %s (%s, CRC32 %08X), save %s: widescreen %s\n", rom.c_str(), g.key.c_str(), crc,
                savePath.c_str(), wide ? "on" : "off (not one of the games EXPAND knows)");
        const int stateAt = getenv("GBAW_TEST_STATE_AT") ? atoi(getenv("GBAW_TEST_STATE_AT")) : -1;
        const std::string statePath = o.saves + "/" + g.key + ".ss1";
        bool smooth = Setting(ini, "scaling") == "smooth";
        bool wideOn = Setting(ini, "widescreen") != "0";
        bool fast = false;
        int volumeStep = (int)(volume * 10 + 0.5f);
        auto saveSettings = [&]() {
            // (Kept beside the DS games' settings; the lines this player does not know stay.)
            std::vector<std::string> lines;
            {
                std::ifstream f(ini);
                std::string l;
                while (std::getline(f, l))
                    if (l.rfind("scaling=", 0) && l.rfind("volume=", 0) && l.rfind("widescreen=", 0) && !l.empty())
                        lines.push_back(l);
            }
            lines.push_back(std::string("scaling=") + (smooth ? "smooth" : "sharp"));
            lines.push_back("volume=" + std::to_string(volumeStep * 10));
            lines.push_back(std::string("widescreen=") + (wideOn ? "1" : "0"));
            std::ofstream f(ini);
            for (const std::string& l : lines) f << l << "\n";
        };
        Menu menu;
        Canvas canvas;
        SDL_Texture* overlay = nullptr;
        int overlayW = 0, overlayH = 0;
        std::string notice;
        int noticeFrames = 0;
        enum { M_RESUME, M_SAVESTATE, M_LOADSTATE, M_FAST, M_WIDE, M_SMOOTH, M_VOLUME, M_RESET, M_QUIT };
        std::vector<int> ids;
        auto build = [&]() {
            menu.items.clear();
            ids.clear();
            auto add = [&](int id, const char* label, const std::string& value, int slider = -1) {
                MenuItem it;
                it.label = label;
                it.value = value;
                it.slider = slider;
                menu.items.push_back(it);
                ids.push_back(id);
            };
            auto onOff = [](bool on) { return std::string(on ? "ON" : "OFF"); };
            add(M_RESUME, "RESUME", "");
            add(M_SAVESTATE, "SAVE STATE", "");
            add(M_LOADSTATE, "LOAD STATE", "");
            add(M_FAST, "FAST FORWARD", onOff(fast));
            if (wide) add(M_WIDE, "WIDESCREEN", onOff(wideOn));
            add(M_SMOOTH, "SMOOTH SCALING", onOff(smooth));
            add(M_VOLUME, "VOLUME", std::to_string(volumeStep * 10) + "%", volumeStep);
            add(M_RESET, "RESET GAME", "");
            add(M_QUIT, "QUIT", "");
        };
        // The GBA's own frame rate, 16777216 / 280896 = 59.7275 a second.
        const Uint64 framePeriod = SDL_GetPerformanceFrequency() * 280896 / 16777216;
        Uint64 nextFrame = SDL_GetPerformanceCounter();
        const bool paced = !maxFrames || getenv("GBAW_TEST_PACED");
        int ran = 0;
        Uint64 measuredFrom = 0;
        unsigned held = 0, prevHeld = 0;
        bool guide = false, rstick = false, zl = false, zr = false, prevCombo = false;
        bool quit = false;
        int lastScene = -1;
        bool wideReady = false;
        unsigned wideW = 240, wideH = 160;
        // (A test run counts the frames it is asked for; a paced one, the frames the game ran.)
        for (int frame = 1; !quit && (!maxFrames || (paced && getenv("GBAW_TEST_PACED") ? ran < maxFrames : frame <= maxFrames)); frame++)
        {
            bool escape = false;
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_QUIT) quit = true;
                else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat)
                {
                    if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) escape = true;
                    const int k = KeyOfKey(e.key.keysym.sym);
                    if (k >= 0) held = e.type == SDL_KEYDOWN ? held | 1u << k : held & ~(1u << k);
                }
                else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP)
                {
                    const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) guide = down;
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) rstick = down;
                    const int k = KeyOfButton(e.cbutton.button);
                    if (k >= 0) held = down ? held | 1u << k : held & ~(1u << k);
                }
                else if (e.type == SDL_CONTROLLERAXISMOTION)
                {
                    if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) zl = e.caxis.value > 16000;
                    if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) zr = e.caxis.value > 16000;
                }
                else if (e.type == SDL_CONTROLLERDEVICEADDED)
                {
                    if (SDL_GameController* p = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(p);
                }
            }
            unsigned keys = held;
            for (const TestEvent& t : script)
                if (frame >= t.first && frame <= t.last) keys |= t.keys;
            // The pause menu: Select and Start together, ZL and ZR together, the right stick pressed,
            // Guide or Escape - never Start alone, which the game needs (its own menu).
            const bool combo = ((keys & (1u << K_SELECT)) && (keys & (1u << K_START))) || (zl && zr) || rstick || guide;
            const unsigned edge = keys & ~prevHeld;
            prevHeld = keys;
            if ((combo && !prevCombo) || escape)
            {
                menu.open = !menu.open;
                menu.selected = 0;
                build();
            }
            prevCombo = combo;
            if (menu.open)
            {
                MenuInput in;
                in.up = edge & (1u << K_UP);
                in.down = edge & (1u << K_DOWN);
                in.left = edge & (1u << K_LEFT);
                in.right = edge & (1u << K_RIGHT);
                in.accept = edge & (1u << K_A);
                in.back = edge & (1u << K_B);
                int dir = 0;
                const int hit = menu.Update(in, dir);
                if (in.back) menu.open = false;
                if (hit >= 0)
                {
                    switch (ids[hit])
                    {
                    case M_RESUME: menu.open = false; break;
                    case M_SAVESTATE:
                        if (struct VFile* sf = VFileOpen(statePath.c_str(), O_CREAT | O_TRUNC | O_WRONLY))
                        {
                            mCoreSaveStateNamed(core, sf, SAVESTATE_RTC);
                            sf->close(sf);
                            notice = "STATE SAVED";
                        }
                        else notice = "STATE NOT SAVED";
                        menu.open = false;
                        break;
                    case M_LOADSTATE:
                        if (struct VFile* sf = VFileOpen(statePath.c_str(), O_RDONLY))
                        {
                            mCoreLoadStateNamed(core, sf, SAVESTATE_RTC);
                            sf->close(sf);
                            pk_wide_reset(); // (EXPAND's memory of the frames before is from before the jump)
                            notice = "STATE LOADED";
                        }
                        else notice = "NO STATE SAVED YET";
                        menu.open = false;
                        break;
                    case M_FAST: fast = !fast; break;
                    case M_WIDE: wideOn = !wideOn; saveSettings(); break;
                    case M_SMOOTH: smooth = !smooth; saveSettings(); break;
                    case M_VOLUME:
                        // (Left and right step it; A goes round, 100% to 0%.)
                        volumeStep = dir ? std::clamp(volumeStep + dir, 0, 10) : (volumeStep + 1) % 11;
                        volume = volumeStep / 10.0f;
                        saveSettings();
                        break;
                    case M_RESET: core->reset(core); pk_wide_reset(); menu.open = false; break;
                    case M_QUIT: quit = true; break;
                    }
                    if (menu.open) build();
                    noticeFrames = notice.empty() ? 0 : 120;
                }
                keys = 0; // (the game sees nothing while the menu is up)
            }
            else if (combo)
                keys &= ~((1u << K_SELECT) | (1u << K_START));

            // The game runs at its own pace (59.73 frames a second): a frame when the sound it made
            // has mostly been played, so the picture never runs ahead of the sound.
            // (Without sound, by the clock: the GBA's 16.78 MHz over 280896 cycles a frame.)
            bool due;
            const Uint64 now = SDL_GetPerformanceCounter();
            if (fast || (maxFrames && !paced)) due = true;
            else
            {
                // By the clock (the GBA's exact 59.7275 a second, not the display's 60 nor the sound
                // queue's drift); the sound only holds a frame back when it is far ahead.
                due = now >= nextFrame && !(audio && SDL_GetQueuedAudioSize(audio) > 6 * 549 * 4);
                if (due) nextFrame = std::max(nextFrame + framePeriod, now - framePeriod);
            }
            if (due && !menu.open && paced && ++ran == 60) measuredFrom = now; // (from the 60th: the start's own delays left out)
            if (!menu.open && due)
            {
                core->setKeys(core, keys);
                for (int i = 0; i < (fast ? 3 : 1); i++)
                {
                    core->runFrame(core);
                    if (wide && wideOn)
                    {
                        int ow = ww, oh = wh;
                        SDL_GetRendererOutputSize(renderer, &ow, &oh);
                        for (int y = 0; y < 160; y++) memcpy(wideSource + y * 240, video + y * 256, 240 * 4);
                        if (i < (fast ? 2 : 0))
                        {
                            pk_wide_skip(core, wideSource);
                            continue;
                        }
                        WideWindow(ow, oh, 240, 160, wideW, wideH);
                        const PkWideView v = { (240 - (int)wideW) / 2, (160 - (int)wideH) / 2, (int)wideW, (int)wideH };
                        pk_wide_view(core, wideSource, wideFrame, v, nullptr);
                        wideReady = !pk_wide_fault();
                        const int scene = pk_wide_scene();
                        if (maxFrames && scene != lastScene) fprintf(stderr, "[gba] frame %d: scene %d\n", frame, scene);
                        lastScene = scene;
                    }
                    else
                        wideReady = false;
                }
                if (fast && audio) SDL_ClearQueuedAudio(audio);
            }
            if (frame == stateAt)
                if (const char* st = getenv("GBAW_TEST_STATE_SAVE"))
                    if (struct VFile* sf = VFileOpen(st, O_CREAT | O_TRUNC | O_WRONLY))
                    {
                        mCoreSaveStateNamed(core, sf, 0);
                        sf->close(sf);
                    }

            int ow = ww, oh = wh;
            SDL_GetRendererOutputSize(renderer, &ow, &oh);
            const bool drawMenu = menu.open || noticeFrames > 0;
            if (drawMenu)
            {
                canvas.Resize(ow, oh);
                canvas.Clear();
                if (menu.open) menu.Draw(canvas, g.name, "A: CHOOSE   B: BACK");
                else
                {
                    const int sc = std::max(1, oh / 240);
                    const int tw = Canvas::TextWidth(notice, sc);
                    canvas.Fill(8 * sc, 8 * sc, tw + 8 * sc, 13 * sc, 0xC0000000);
                    canvas.Text(12 * sc, 11 * sc, notice, sc, 0xFFFFFFFF);
                    noticeFrames--;
                }
            }
            const bool shot = std::find(shots.begin(), shots.end(), frame) != shots.end();
            if (shot)
            {
                char name[64];
                snprintf(name, sizeof name, "/gba_%06d.bmp", frame);
                if (wideReady) SaveBmp(testDir + name, wideFrame, (int)wideW, (int)wideH, (int)wideW);
                else SaveBmp(testDir + name, video, 240, 160, 256);
#ifndef __SWITCH__
                if (drawMenu)
                {
                    snprintf(name, sizeof name, "/menu_%06d.bmp", frame);
                    if (SDL_Surface* m = SDL_CreateRGBSurfaceWithFormatFrom(canvas.px.data(), canvas.w, canvas.h, 32,
                                                                           canvas.w * 4, SDL_PIXELFORMAT_ARGB8888))
                    {
                        SDL_SaveBMP(m, (testDir + name).c_str());
                        SDL_FreeSurface(m);
                    }
                }
#endif
            }
            if (maxFrames && !shot)
            {
                if (paced && !due) SDL_Delay(1);
                continue; // (a test run draws only what it keeps)
            }
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, smooth ? "1" : "0");
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_Rect src = { 0, 0, 240, 160 };
            SDL_Texture* tex = gameTex;
            if (wideReady)
            {
                SDL_UpdateTexture(wideTex, nullptr, wideFrame, (int)wideW * 4);
                src = { 0, 0, (int)wideW, (int)wideH };
                tex = wideTex;
            }
            else
                SDL_UpdateTexture(gameTex, nullptr, video, 256 * 4);
            SDL_SetTextureScaleMode(tex, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
            // The picture as large as the screen allows, its shape kept.
            const float scale = std::min((float)ow / src.w, (float)oh / src.h);
            const int dw = (int)(src.w * scale), dh = (int)(src.h * scale);
            const SDL_Rect dst = { (ow - dw) / 2, (oh - dh) / 2, dw, dh };
            SDL_RenderCopy(renderer, tex, &src, &dst);
            if (drawMenu)
            {
                if (!overlay || overlayW != ow || overlayH != oh)
                {
                    if (overlay) SDL_DestroyTexture(overlay);
        if (maxFrames && paced)
        {
            const double secs = (double)(SDL_GetPerformanceCounter() - measuredFrom) / SDL_GetPerformanceFrequency();
            fprintf(stderr, "[gba] %d frames in %.2f s: %.3f a second (the GBA: 59.727)\n", ran - 60, secs, (ran - 60) / secs);
        }
                    overlay = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
                    if (overlay) SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
                    overlayW = ow;
                    overlayH = oh;
                }
                if (overlay)
                {
                    SDL_UpdateTexture(overlay, nullptr, canvas.px.data(), canvas.w * 4);
                    SDL_RenderCopy(renderer, overlay, nullptr, nullptr);
                }
            }
            SDL_RenderPresent(renderer);
        }
        if (overlay) SDL_DestroyTexture(overlay);
        if (maxFrames && paced)
        {
            const double secs = (double)(SDL_GetPerformanceCounter() - measuredFrom) / SDL_GetPerformanceFrequency();
            fprintf(stderr, "[gba] %d frames in %.2f s: %.3f a second (the GBA: 59.727)\n", ran - 60, secs, (ran - 60) / secs);
        }
        pk_wide_configure(0);
        // (mGBA writes the save as it goes; deinit flushes the rest.)
        core->deinit(core);
        fprintf(stderr, "[gba] left %s\n", rom.c_str());
    }
    for (SDL_GameController* p : pads) SDL_GameControllerClose(p);
    if (audio) SDL_CloseAudioDevice(audio);
    audio = 0;
    SDL_DestroyTexture(wideTex);
    SDL_DestroyTexture(gameTex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

}
