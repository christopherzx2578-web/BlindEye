// ═══════════════════════════════════════════════════════════════════════
//  Arma Reforger ESP Overlay v2 (Kernel-Assisted)
//  Integrates with BlindEye kernel driver for anti-cheat evasion
//  
//  Build: Visual Studio 2022 x64 Release
//  Link: gdiplus.lib dwmapi.lib Psapi.lib ntdll.lib setupapi.lib
//
//  Hotkeys: F1 Box  F2 Skeleton  F3 Health  F4 Name  F5 Distance
//           F6 Snap F7 HeadDot   F8 Faction F9 INCAP  F10 DEAD
//           INS Radar   END Quit
// ═══════════════════════════════════════════════════════════════════════

#define _USE_MATH_DEFINES

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <dwmapi.h>
#include <setupapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <random>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "setupapi.lib")

#include "SharedTypes.h"

using namespace Gdiplus;

// ───────────────────────────────────────────────────────────────────────
//  DEVICE COMMUNICATION
// ───────────────────────────────────────────────────────────────────────

static HANDLE g_DriverHandle = INVALID_HANDLE_VALUE;

static bool OpenKernelDriver()
{
    g_DriverHandle = CreateFileW(
        L"\\\\.\\BlindEyeESP",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    return g_DriverHandle != INVALID_HANDLE_VALUE;
}

static void CloseKernelDriver()
{
    if (g_DriverHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_DriverHandle);
        g_DriverHandle = INVALID_HANDLE_VALUE;
    }
}

// Kernel-assisted memory read
static bool KernelRead(uintptr_t addr, void* buf, size_t len)
{
    if (g_DriverHandle == INVALID_HANDLE_VALUE)
        return false;

    BLINDEYE_READ_REQUEST req{ addr, (ULONG)len, 0 };
    BLINDEYE_READ_RESPONSE resp{};
    DWORD bytesRet = 0;

    BOOL ok = DeviceIoControl(g_DriverHandle, IOCTL_BLINDEYE_READ_MEMORY,
                               &req, sizeof(req), &resp, sizeof(resp),
                               &bytesRet, nullptr);

    if (ok && resp.BytesRead == len) {
        RtlCopyMemory(buf, resp.Data, len);
        return true;
    }
    return false;
}

// ───────────────────────────────────────────────────────────────────────
//  MATH
// ───────────────────────────────────────────────────────────────────────
struct Vec3 { float x{}, y{}, z{}; };
struct Vec2 { float x{}, y{}; };

static Vec3  operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static float Dot(Vec3 a, Vec3 b)       { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float Dist3D(Vec3 a, Vec3 b) {
    float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

struct CamData {
    Vec3  pos{}, right{}, up{}, forward{};
    float fovRad{}, zoom{};
    Vec3  zoomFactor{};
};

static bool W2S(Vec3 world, Vec2& scr, const CamData& c, float W, float H)
{
    Vec3  d = world - c.pos;
    float X = Dot(d, c.right), Y = Dot(d, c.up), Z = Dot(d, c.forward);
    if (Z <= 0.001f) return false;
    float fov = c.fovRad * (180.f / 3.14159265f);
    if (fov < 1.f) fov = 70.f;
    if (c.zoom > 0.f) {
        fov = 90.f;
        X /= (c.zoomFactor.x ? c.zoomFactor.x : 1.f);
        Y /= (c.zoomFactor.y ? c.zoomFactor.y : 1.f);
    }
    float t = 1.f / tanf(fov * 0.5f * (3.14159265f / 180.f));
    float sx = (X / Z) * t / (W / H);
    float sy = (Y / Z) * t;
    scr.x = (sx + 1.f) * 0.5f * W;
    scr.y = (1.f - sy) * 0.5f * H;
    return scr.x > 0.f && scr.x < W && scr.y > 0.f && scr.y < H;
}

// ───────────────────────────────────────────────────────────────────────
//  MEMORY
// ───────────────────────────────────────────────────────────────────────
static HANDLE g_proc = nullptr;
static bool   g_useKernel = false;

static HANDLE OpenGameProcess(DWORD pid)
{
    const DWORD access = PROCESS_VM_READ | PROCESS_QUERY_INFORMATION;
    return OpenProcess(access, FALSE, pid);
}

template<typename T>
static T RPM(uintptr_t addr)
{
    T v{};
    if (!addr || !g_proc) return v;
    
    // Try kernel-assisted read first
    if (g_useKernel && KernelRead(addr, &v, sizeof(T)))
        return v;
    
    // Fallback to user-mode read
    ReadProcessMemory(g_proc, (LPCVOID)addr, &v, sizeof(T), nullptr);
    return v;
}

static std::string RPMStr(uintptr_t addr, size_t cap = 64)
{
    if (!addr || !g_proc) return {};
    std::string s(cap, '\0');
    
    if (g_useKernel)
        KernelRead(addr, s.data(), cap);
    else
        ReadProcessMemory(g_proc, (LPCVOID)addr, s.data(), cap, nullptr);
    
    s.resize(strnlen(s.c_str(), cap));
    return s;
}

static bool RPMBuf(uintptr_t addr, void* buf, size_t len)
{
    if (!addr || !g_proc) return false;
    
    if (g_useKernel && KernelRead(addr, buf, len))
        return true;
    
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (LPCVOID)addr, buf, len, &got) && got == len;
}

// ───────────────────────────────────────────────────────────────────────
//  PROCESS DISCOVERY
// ───────────────────────────────────────────────────────────────────────
static DWORD FindPID(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) }; DWORD pid = 0;
    if (Process32FirstW(snap, &pe))
        do { if (!_wcsicmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

// ───────────────────────────────────────────────────────────────────────
//  SIGNATURE SCANNER
// ───────────────────────────────────────────────────────────────────────
struct SigByte { uint8_t v; bool wild; };
using Pattern = std::vector<SigByte>;

static Pattern MakePat(const char* s)
{
    Pattern p;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (s[0] == '?' && s[1] == '?') { p.push_back({0, true}); s += 2; }
        else {
            char h[3] = {s[0], s[1], 0};
            p.push_back({(uint8_t)strtoul(h, nullptr, 16), false});
            s += 2;
        }
    }
    return p;
}

static bool GetTextSection(uintptr_t base, uintptr_t& start, size_t& size)
{
    IMAGE_DOS_HEADER dos{};
    if (!RPMBuf(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!RPMBuf(base + dos.e_lfanew, &nt, sizeof(nt))) return false;
    auto* sec = IMAGE_FIRST_SECTION(&nt);
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; i++, sec++) {
        char name[9]{}; memcpy(name, sec->Name, 8);
        if (!strcmp(name, ".text")) { start = base + sec->VirtualAddress; size = sec->Misc.VirtualSize; return true; }
    }
    return false;
}

static uintptr_t ScanRemote(uintptr_t rgnBase, size_t rgnSize, const Pattern& pat)
{
    constexpr size_t CHUNK = 0x1000;
    size_t pLen = pat.size();
    std::vector<uint8_t> buf(CHUNK + pLen);

    for (size_t off = 0; off + pLen <= rgnSize; off += CHUNK) {
        size_t readSz = std::min(CHUNK + pLen - 1, rgnSize - off);
        if (!RPMBuf(rgnBase + off, buf.data(), readSz)) continue;
        for (size_t i = 0; i + pLen <= readSz; i++) {
            bool ok = true;
            for (size_t j = 0; j < pLen; j++)
                if (!pat[j].wild && buf[i + j] != pat[j].v) { ok = false; break; }
            if (ok) return rgnBase + off + i;
        }
    }
    return 0;
}

static uintptr_t RipRel(uintptr_t hit, int dispOff, int instrLen)
{
    int32_t disp = RPM<int32_t>(hit + dispOff);
    return hit + instrLen + disp;
}

// ───────────────────────────────────────────────────────────────────────
//  OFFSETS
// ───────────────────────────────────────────────────────────────────────
struct Offsets {
    uintptr_t gamePtr        = 0;
    uintptr_t world          = 0;
    uintptr_t playerMgr      = 0;
    uintptr_t entityList     = 0;
    uintptr_t entityCount    = 0;
    uintptr_t localCtrlWeak  = 0;
    uintptr_t weakObj        = 0x8;
    uintptr_t localEntNeg    = 0x10;
    uintptr_t entityPos      = 0;
    uintptr_t charCtrl       = 0;
    uintptr_t lifeState      = 0;
    uintptr_t factionWeak    = 0;
    uintptr_t factionStr     = 0x68;
    uintptr_t dmgMgr         = 0;
    uintptr_t hitzone        = 0x70;
    uintptr_t maxHP          = 0x40;
    uintptr_t curHP          = 0x44;
    uintptr_t meshComp       = 0x50;
    uintptr_t meshData       = 0x18;
    uintptr_t meshObj        = 0x30;
    uintptr_t boneArr        = 0x40;
    uintptr_t boneStride     = 0x30;
    int       headBone       = 83;
    uintptr_t camMgrWeak     = 0;
    uintptr_t camWeak        = 0;
    uintptr_t camPos         = 0x58;
    uintptr_t camRight       = 0x70;
    uintptr_t camUp          = 0x7C;
    uintptr_t camFwd         = 0x88;
    uintptr_t camFov         = 0x128;
    uintptr_t camZoom        = 0x1DC;
    uintptr_t camZoomFac     = 0x18C;
    uintptr_t identList      = 0x18;
    uintptr_t identName      = 0x18;
    uintptr_t identEntWeak   = 0x48;
    bool      valid          = false;
};

static Offsets ScanOffsets(uintptr_t base)
{
    Offsets o;
    uintptr_t ts = 0; size_t tSz = 0;
    if (!GetTextSection(base, ts, tSz)) return o;

    {
        auto p = MakePat("48 8B 05 ?? ?? ?? ?? 48 85 C0 74");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.gamePtr = RipRel(h, 3, 7);
    }
    {
        auto p = MakePat("48 8B 8B ?? ?? 00 00 48 85 C9 74");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.world = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
    }
    {
        auto p = MakePat("4C 8B 87 ?? ?? 00 00 8B 8F ?? ?? 00 00");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) {
            o.entityList  = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
            o.entityCount = (uintptr_t)(int32_t)RPM<int32_t>(h + 10);
        }
    }
    {
        auto p = MakePat("48 8B B3 ?? ?? 00 00 48 8B CE");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.localCtrlWeak = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
    }
    {
        auto p = MakePat("F3 0F 10 86 ?? ?? 00 00 F3 0F 10 8E ?? ?? 00 00");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.entityPos = (uintptr_t)(int32_t)RPM<int32_t>(h + 4);
    }
    {
        auto p = MakePat("48 8B 96 ?? ?? 00 00 48 85 D2 74 ?? F6 82");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.charCtrl = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
    }
    {
        auto p = MakePat("8B 81 ?? ?? 00 00 83 F8 02 74");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.lifeState = (uintptr_t)(int32_t)RPM<int32_t>(h + 2);
    }
    {
        auto p = MakePat("48 8B B8 ?? ?? 00 00 48 85 FF 74 ?? 48 8B 5F 08");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.factionWeak = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
    }
    {
        auto p = MakePat("48 8B 8E ?? ?? 00 00 48 85 C9 74 ?? F3 0F 10 41");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.dmgMgr = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
    }
    {
        auto p = MakePat("48 8B 83 ?? ?? 00 00 48 85 C0 74 ?? 48 8B 80 ?? ?? 00 00");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) {
            o.camMgrWeak = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
            o.camWeak    = (uintptr_t)(int32_t)RPM<int32_t>(h + 15);
        }
    }
    {
        auto p = MakePat("48 8B 83 ?? ?? 00 00 48 8B 40 18 48 85 C0 74");
        uintptr_t h = ScanRemote(ts, tSz, p);
        if (h) o.playerMgr = (uintptr_t)(int32_t)RPM<int32_t>(h + 3);
    }

    o.valid = (o.gamePtr != 0 && o.world != 0 && o.entityList != 0);
    return o;
}

static Offsets FallbackOffsets(uintptr_t base)
{
    Offsets o;
    o.gamePtr       = base + 0x2142878;
    o.world         = 0x130;
    o.playerMgr     = 0x2D0;
    o.entityList    = 0x128;
    o.entityCount   = 0x134;
    o.localCtrlWeak = 0x378;
    o.entityPos     = 0xA4;
    o.charCtrl      = 0xF8;
    o.lifeState     = 0x9A0;
    o.factionWeak   = 0x188;
    o.dmgMgr        = 0x130;
    o.camMgrWeak    = 0x318;
    o.camWeak       = 0x108;
    o.valid         = true;
    return o;
}

// ───────────────────────────────────────────────────────────────────────
//  ENTITY / FEATURES
// ───────────────────────────────────────────────────────────────────────
enum class Faction   { UNKNOWN, BLUFOR, OPFOR, INDFOR };
enum class LifeState { ALIVE, INCAP, DEAD };

struct Entity {
    uintptr_t  ptr{};
    Vec3       worldPos{}, headPos{};
    float      curHP{100.f}, maxHP{100.f};
    LifeState  life{LifeState::ALIVE};
    Faction    faction{Faction::UNKNOWN};
    std::string name;
    float      dist{};
    Vec2       scr{}, scrHead{};
    bool       onScr{}, headOnScr{};
};

struct Feats {
    std::atomic<bool> box      {true};
    std::atomic<bool> skeleton {false};
    std::atomic<bool> health   {true};
    std::atomic<bool> name     {true};
    std::atomic<bool> distance {true};
    std::atomic<bool> snap     {false};
    std::atomic<bool> headDot  {true};
    std::atomic<bool> faction  {true};
    std::atomic<bool> incap    {true};
    std::atomic<bool> dead     {false};
    std::atomic<bool> radar    {true};
    std::atomic<int>  maxDist  {800};
} g_feat;

static std::mutex           g_lock;
static std::vector<Entity>  g_ents;
static CamData              g_cam;
static std::atomic<bool>    g_run     {true};
static std::atomic<bool>    g_attached{false};
static std::atomic<bool>    g_scanning{false};
static std::atomic<float>   g_sw{1920.f}, g_sh{1080.f};
static std::wstring         g_statusMsg = L"Waiting...";
static std::mutex           g_statusLock;

static void SetStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(g_statusLock);
    g_statusMsg = s;
}

// ───────────────────────────────────────────────────────────────────────
//  PALETTE
// ───────────────────────────────────────────────────────────────────────
static const Color cBLUFOR(255, 59,130,246);
static const Color cOPFOR (255,239, 68, 68);
static const Color cINDFOR(255,245,158, 11);
static const Color cDead  (255, 80, 80, 80);
static const Color cUnk   (255,160,160,160);
static const Color cWhite (255,255,255,255);
static const Color cGreen (255, 34,197, 94);
static const Color cAmber (255,245,158, 11);
static const Color cRed   (255,239, 68, 68);
static const Color cYellow(255,251,191, 36);
static const Color cGray  (180,148,163,184);

static Color FacCol(Faction f, bool dead) {
    if (dead) return cDead;
    switch (f) {
        case Faction::BLUFOR: return cBLUFOR;
        case Faction::OPFOR:  return cOPFOR;
        case Faction::INDFOR: return cINDFOR;
        default:              return cUnk;
    }
}

static Color HPCol(float p) {
    return p > 0.6f ? cGreen : p > 0.3f ? cAmber : cRed;
}

// ───────────────────────────────────────────────────────────────────────
//  RENDER HELPERS
// ───────────────────────────────────────────────────────────────────────
static void DrawStr(Graphics& g, const std::wstring& txt, float x, float y,
                    const Font& f, Color c)
{
    SolidBrush sh(Color(180,0,0,0));
    for (auto& p : std::vector<PointF>{{x-1,y},{x+1,y},{x,y-1},{x,y+1}})
        g.DrawString(txt.c_str(), -1, &f, p, &sh);
    SolidBrush br(c);
    g.DrawString(txt.c_str(), -1, &f, PointF(x, y), &br);
}

static void DrawBox(Graphics& g, float x, float y, float w, float h, Color c, float t=1.5f)
{
    Pen dim(Color(80, c.GetR(), c.GetG(), c.GetB()), t);
    g.DrawRectangle(&dim, x, y, w, h);
    Pen cp(Color(255, c.GetR(), c.GetG(), c.GetB()), t + 0.5f);
    float s = std::max(w,h) * 0.22f;
    g.DrawLine(&cp, x,   y,   x+s, y  ); g.DrawLine(&cp, x,   y,   x,   y+s);
    g.DrawLine(&cp, x+w, y,   x+w-s,y ); g.DrawLine(&cp, x+w, y,   x+w, y+s);
    g.DrawLine(&cp, x,   y+h, x+s, y+h); g.DrawLine(&cp, x,   y+h, x,   y+h-s);
    g.DrawLine(&cp, x+w, y+h, x+w-s,y+h);g.DrawLine(&cp, x+w, y+h, x+w, y+h-s);
}

// ───────────────────────────────────────────────────────────────────────
//  RENDER
// ───────────────────────────────────────────────────────────────────────
static void Render(Graphics& gfx, float sw, float sh)
{
    std::vector<Entity> ents;
    CamData cam;
    { std::lock_guard<std::mutex> lk(g_lock); ents = g_ents; cam = g_cam; }

    FontFamily ff(L"Consolas");
    Font fMain(&ff, 11.f, FontStyleRegular, UnitPixel);
    Font fSm  (&ff,  9.f, FontStyleRegular, UnitPixel);
    Font fBold(&ff, 13.f, FontStyleBold,    UnitPixel);
    Font fTiny(&ff,  8.f, FontStyleRegular, UnitPixel);

    if (!g_attached) {
        std::wstring status;
        { std::lock_guard<std::mutex> lk(g_statusLock); status = g_statusMsg; }
        DrawStr(gfx, L"[REFORGER ESP v2]  " + status, 12.f, 12.f, fBold, Color(255,239,68,68));
        DrawStr(gfx, g_useKernel ? L"Kernel-Assisted Mode" : L"User-Mode Fallback",
                12.f, 30.f, fSm, g_useKernel ? cGreen : cAmber);
        return;
    }

    // HUD strip
    {
        const struct { const wchar_t* lbl; bool on; } fl[] = {
            {L"F1  Box ESP",    (bool)g_feat.box},
            {L"F2  Skeleton",   (bool)g_feat.skeleton},
            {L"F3  Health",     (bool)g_feat.health},
            {L"F4  Name",       (bool)g_feat.name},
            {L"F5  Distance",   (bool)g_feat.distance},
            {L"F6  Snap Lines", (bool)g_feat.snap},
            {L"F7  Head Dot",   (bool)g_feat.headDot},
            {L"F8  Faction",    (bool)g_feat.faction},
            {L"F9  INCAP",      (bool)g_feat.incap},
            {L"F10 DEAD",       (bool)g_feat.dead},
            {L"INS Radar",      (bool)g_feat.radar},
            {L"END Quit",       false},
        };
        float hx = 10.f, hy = 10.f;
        SolidBrush bg(Color(140,10,10,10));
        gfx.FillRectangle(&bg, hx-4.f, hy-4.f, 130.f, 178.f);
        DrawStr(gfx, L"REFORGER ESP v2", hx, hy, fBold, g_useKernel ? cGreen : cRed); hy += 16.f;
        for (auto& f2 : fl) {
            Color fc = f2.on ? cGreen : Color(120,90,90,90);
            DrawStr(gfx, std::wstring(f2.lbl) + (f2.on ? L" ●" : L" ○"), hx, hy, fTiny, fc);
            hy += 12.f;
        }
        std::wstringstream ss; ss << L"Targets: " << ents.size();
        DrawStr(gfx, ss.str(), hx, hy+3.f, fTiny, cGray);
    }

    // Radar
    if (g_feat.radar) {
        const float rw=210.f, rh=210.f, rx=sw-rw-14.f, ry=14.f, range=350.f;
        SolidBrush bg(Color(155,8,8,8)); Pen border(Color(100,90,90,90),1.f);
        gfx.FillRectangle(&bg,rx,ry,rw,rh); gfx.DrawRectangle(&border,rx,ry,rw,rh);
        for (float r : {60.f,120.f,180.f}) {
            Pen rp(Color(35,120,120,120),1.f);
            gfx.DrawEllipse(&rp, rx+rw/2-r/2, ry+rh/2-r/2, r, r);
        }
        { Pen cp(Color(30,120,120,120),1.f);
          gfx.DrawLine(&cp,rx,ry+rh/2,rx+rw,ry+rh/2);
          gfx.DrawLine(&cp,rx+rw/2,ry,rx+rw/2,ry+rh); }
        SolidBrush lp(cWhite); gfx.FillEllipse(&lp,rx+rw/2-4.f,ry+rh/2-4.f,8.f,8.f);
        for (auto& e : ents) {
            if (e.life == LifeState::DEAD && !g_feat.dead) continue;
            Color dc = FacCol(e.faction, e.life == LifeState::DEAD);
            Vec3 d = e.worldPos - cam.pos;
            float dotX = std::clamp(rx+rw/2.f+d.x/range*(rw/2.f), rx+2.f, rx+rw-2.f);
            float dotY = std::clamp(ry+rh/2.f-d.z/range*(rh/2.f), ry+2.f, ry+rh-2.f);
            float ds = (e.life==LifeState::DEAD) ? 4.f : 5.f;
            SolidBrush db(dc); gfx.FillEllipse(&db, dotX-ds, dotY-ds, ds*2.f, ds*2.f);
        }
    }

    // Per-entity
    for (auto& e : ents) {
        if (!e.onScr) continue;
        bool dead = e.life==LifeState::DEAD, incap = e.life==LifeState::INCAP;
        Color col = dead ? cDead : incap ? cAmber : FacCol(e.faction, false);

        float eH = std::clamp(e.scr.y - e.scrHead.y, 8.f, 900.f);
        float bW  = eH * 0.42f;
        float bX  = e.scrHead.x - bW/2.f;
        float bY  = e.scrHead.y - eH*0.04f;

        if (g_feat.snap) {
            Pen sp(Color(100,col.GetR(),col.GetG(),col.GetB()),1.f);
            gfx.DrawLine(&sp, sw/2.f, sh, e.scrHead.x, e.scrHead.y);
        }
        if (g_feat.box) DrawBox(gfx, bX, bY, bW, eH, col);

        if (g_feat.health) {
            float pw = 4.f, px = bX-pw-2.f;
            float pct = dead ? 0.f : std::clamp(e.curHP/e.maxHP, 0.f, 1.f);
            SolidBrush bg2(Color(140,10,10,10)); gfx.FillRectangle(&bg2,px,bY,pw,eH);
            SolidBrush fg(HPCol(pct)); float fH=eH*pct;
            gfx.FillRectangle(&fg, px, bY+(eH-fH), pw, fH);
            Pen bp(Color(60,80,80,80),0.5f); gfx.DrawRectangle(&bp,px,bY,pw,eH);
        }
        if (g_feat.headDot && e.headOnScr) {
            SolidBrush hd(cYellow); Pen hp(Color(200,0,0,0),1.f);
            gfx.FillEllipse(&hd, e.scrHead.x-4.f, e.scrHead.y-4.f, 8.f, 8.f);
            gfx.DrawEllipse(&hp, e.scrHead.x-4.f, e.scrHead.y-4.f, 8.f, 8.f);
        }

        float ly = bY - 1.f;
        if (g_feat.name) {
            ly -= 12.f;
            DrawStr(gfx, std::wstring(e.name.begin(), e.name.end()), bX, ly, fMain, col);
        }
        if (g_feat.faction) {
            ly -= 11.f;
            std::wstring tag;
            switch (e.faction) {
                case Faction::BLUFOR: tag=L"BLUFOR"; break;
                case Faction::OPFOR:  tag=L"OPFOR";  break;
                case Faction::INDFOR: tag=L"INDFOR"; break;
                default:              tag=L"???";    break;
            }
            if (dead) tag+=L" ☠"; if (incap) tag+=L" INCAP";
            DrawStr(gfx, tag, bX, ly, fTiny, FacCol(e.faction, dead));
        }
        if (g_feat.distance) {
            std::wstringstream ds; ds << (int)e.dist << L"m";
            DrawStr(gfx, ds.str(), bX, e.scr.y+3.f, fTiny, cGray);
        }
        if (g_feat.health && !dead) {
            std::wstringstream hs; hs << (int)(e.curHP/e.maxHP*100.f) << L"%";
            DrawStr(gfx, hs.str(), bX+bW/2.f-10.f, bY+eH-13.f, fTiny, HPCol(e.curHP/e.maxHP));
        }
    }
}

// ───────────────────────────────────────────────────────────────────────
//  READER THREAD
// ───────────────────────────────────────────────────────────────────────
static const wchar_t* kProcNames[] = {
    L"ArmaReforgerSteam.exe",
    L"ArmaReforger.exe",
    nullptr
};

static void ReaderThread()
{
    std::mt19937 rng(std::random_device{}());
    auto jit = [&](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo,hi)(rng);
    };

    SetStatus(L"Attempting kernel driver connection...");
    g_useKernel = OpenKernelDriver();
    if (g_useKernel)
        SetStatus(L"Kernel driver connected (anti-cheat evasion enabled)");
    else
        SetStatus(L"Kernel driver unavailable (user-mode fallback)");

    SetStatus(L"Scanning for Arma Reforger...");

    while (g_run) {
        DWORD pid = 0;
        const wchar_t* foundName = nullptr;
        for (int i = 0; kProcNames[i] && !pid; i++) {
            pid = FindPID(kProcNames[i]);
            if (pid) foundName = kProcNames[i];
        }
        if (!pid) {
            SetStatus(L"Game not found");
            g_attached=false; Sleep(jit(1800,2400)); continue;
        }

        HANDLE h = OpenGameProcess(pid);
        if (!h) {
            SetStatus(L"OpenProcess failed");
            g_attached=false; Sleep(jit(900,1200)); continue;
        }
        g_proc = h;
        SetStatus(L"Found game, scanning signatures...");

        g_scanning = true;
        uintptr_t base = 0x140000000; // Standard Reforger base
        Offsets off = ScanOffsets(base);
        if (!off.valid)
            off = FallbackOffsets(base);
        g_scanning = false;

        if (!off.valid) {
            SetStatus(L"Offset scan failed");
            CloseHandle(h); g_proc=nullptr; Sleep(jit(3000,4000)); continue;
        }
        SetStatus(g_useKernel ? L"ATTACHED (Kernel-Assisted)" : L"ATTACHED (User-Mode)");
        g_attached = true;

        while (g_run) {
            DWORD ec = 0;
            if (!GetExitCodeProcess(h, &ec) || ec != STILL_ACTIVE) break;

            uintptr_t game = RPM<uintptr_t>(off.gamePtr);
            if (!game) { Sleep(200); continue; }
            uintptr_t world = RPM<uintptr_t>(game + off.world);
            if (!world) { Sleep(200); continue; }

            uintptr_t cmWeak = RPM<uintptr_t>(game + off.camMgrWeak);
            uintptr_t cmgr   = RPM<uintptr_t>(cmWeak + off.weakObj);
            uintptr_t cwWeak = RPM<uintptr_t>(cmgr + off.camWeak);
            uintptr_t camPtr = RPM<uintptr_t>(cwWeak + off.weakObj);

            CamData cam{};
            cam.pos        = RPM<Vec3>(camPtr + off.camPos);
            cam.right      = RPM<Vec3>(camPtr + off.camRight);
            cam.up         = RPM<Vec3>(camPtr + off.camUp);
            cam.forward    = RPM<Vec3>(camPtr + off.camFwd);
            cam.fovRad     = RPM<float>(cmgr  + off.camFov);
            cam.zoom       = RPM<float>(camPtr + off.camZoom);
            cam.zoomFactor = RPM<Vec3>(camPtr + off.camZoomFac);

            uintptr_t lcW  = RPM<uintptr_t>(world + off.localCtrlWeak);
            uintptr_t lcC  = RPM<uintptr_t>(lcW + off.weakObj);
            uintptr_t lcE  = lcC ? RPM<uintptr_t>(lcC - off.localEntNeg) : 0;

            uintptr_t eList  = RPM<uintptr_t>(world + off.entityList);
            int       eCount = std::clamp(RPM<int>(world + off.entityCount), 0, 1024);

            float sw2 = g_sw, sh2 = g_sh;
            std::vector<Entity> frame; frame.reserve(64);

            for (int i = 0; i < eCount; i++) {
                uintptr_t ep = RPM<uintptr_t>(eList + (uintptr_t)i*8);
                if (!ep || ep == lcE) continue;

                Entity e; e.ptr = ep;
                uintptr_t ctrl = RPM<uintptr_t>(ep + off.charCtrl);
                int32_t ls = ctrl ? RPM<int32_t>(ctrl + off.lifeState) : 0;
                e.life = ls==2 ? LifeState::DEAD : ls==1 ? LifeState::INCAP : LifeState::ALIVE;

                if (e.life==LifeState::DEAD  && !g_feat.dead)  continue;
                if (e.life==LifeState::INCAP && !g_feat.incap) continue;

                e.worldPos = RPM<Vec3>(ep + off.entityPos);
                e.dist     = Dist3D(e.worldPos, cam.pos);
                if (e.dist > (float)g_feat.maxDist) continue;

                uintptr_t fw = RPM<uintptr_t>(ep + off.factionWeak);
                uintptr_t fo = RPM<uintptr_t>(fw + off.weakObj);
                std::string fs = fo ? RPMStr(fo + off.factionStr, 32) : "";
                auto ci = [&](const char* s){ return fs.find(s)!=std::string::npos; };
                if      (ci("US")||ci("Blue")||ci("NATO"))  e.faction=Faction::BLUFOR;
                else if (ci("USSR")||ci("Red")||ci("CSAT")) e.faction=Faction::OPFOR;
                else if (ci("FIA")||ci("Ind")||ci("Green")) e.faction=Faction::INDFOR;

                uintptr_t dmg = RPM<uintptr_t>(ep + off.dmgMgr);
                uintptr_t hz  = RPM<uintptr_t>(dmg + off.hitzone);
                e.maxHP = hz ? RPM<float>(hz+off.maxHP) : 100.f;
                e.curHP = hz ? RPM<float>(hz+off.curHP) : 0.f;
                if (e.maxHP <= 0.f) e.maxHP = 100.f;

                e.name = "Enemy";
                e.headPos = {e.worldPos.x, e.worldPos.y+1.7f, e.worldPos.z};

                e.onScr     = W2S(e.worldPos, e.scr,     cam, sw2, sh2);
                e.headOnScr = W2S(e.headPos,  e.scrHead, cam, sw2, sh2);
                frame.push_back(std::move(e));
            }

            { std::lock_guard<std::mutex> lk(g_lock); g_ents=std::move(frame); g_cam=cam; }
            Sleep(jit(7,12));
        }

        CloseHandle(h); g_proc=nullptr; g_attached=false;
        Sleep(jit(800,1200));
    }
}

// ───────────────────────────────────────────────────────────────────────
//  HOTKEY THREAD
// ───────────────────────────────────────────────────────────────────────
static void HotkeyThread()
{
    bool prev[256]{};
    auto edge = [&](int vk) {
        bool c=(GetAsyncKeyState(vk)&0x8000)!=0, f=c&&!prev[vk]; prev[vk]=c; return f;
    };
    while (g_run) {
        auto tog=[](std::atomic<bool>& f){ f.store(!f.load()); };
        if (edge(VK_F1))     tog(g_feat.box);
        if (edge(VK_F2))     tog(g_feat.skeleton);
        if (edge(VK_F3))     tog(g_feat.health);
        if (edge(VK_F4))     tog(g_feat.name);
        if (edge(VK_F5))     tog(g_feat.distance);
        if (edge(VK_F6))     tog(g_feat.snap);
        if (edge(VK_F7))     tog(g_feat.headDot);
        if (edge(VK_F8))     tog(g_feat.faction);
        if (edge(VK_F9))     tog(g_feat.incap);
        if (edge(VK_F10))    tog(g_feat.dead);
        if (edge(VK_INSERT)) tog(g_feat.radar);
        if (edge(VK_END))    { g_run=false; PostQuitMessage(0); }
        Sleep(10);
    }
}

// ───────────────────────────────────────────────────────────────────────
//  WINDOW
// ───────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { g_run=false; PostQuitMessage(0); return 0; }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        float w=(float)rc.right, h=(float)rc.bottom; g_sw=w; g_sh=h;
        HDC mem=CreateCompatibleDC(hdc);
        HBITMAP bm=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
        auto old=SelectObject(mem,bm);
        HBRUSH clr=CreateSolidBrush(RGB(0,0,0)); FillRect(mem,&rc,clr); DeleteObject(clr);
        { Graphics gfx(mem);
          gfx.SetSmoothingMode(SmoothingModeHighSpeed);
          gfx.SetTextRenderingHint(TextRenderingHintSingleBitPerPixelGridFit);
          Render(gfx, w, h); }
        BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,old); DeleteDC(mem); DeleteObject(bm);
        EndPaint(hwnd,&ps); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ───────────────────────────────────────────────────────────────────────
//  ENTRY
// ───────────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    GdiplusStartupInput gsi; ULONG_PTR tok;
    GdiplusStartup(&tok, &gsi, nullptr);

    g_sw=(float)GetSystemMetrics(SM_CXSCREEN);
    g_sh=(float)GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.lpszClassName=L"ReforgerESP_v2";
    wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW,
        L"ReforgerESP_v2", L"", WS_POPUP,
        0, 0, (int)g_sw.load(), (int)g_sh.load(),
        nullptr, nullptr, hInst, nullptr);

    SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 0, LWA_COLORKEY);
    MARGINS m{-1}; DwmExtendFrameIntoClientArea(hwnd, &m);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE); UpdateWindow(hwnd);

    std::thread reader(ReaderThread), hotkey(HotkeyThread);

    MSG msg{};
    while (g_run) {
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if (msg.message==WM_QUIT) { g_run=false; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!g_run) break;
        InvalidateRect(hwnd, nullptr, FALSE);
        Sleep(7);
    }

    reader.join(); hotkey.join();
    CloseKernelDriver();
    GdiplusShutdown(tok);
    return 0;
}
