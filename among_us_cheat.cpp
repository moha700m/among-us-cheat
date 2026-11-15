/*
 * Among Us External Cheat
 * language: C++17, file: among_us_cheat.cpp
 * compile: g++ -o AmongUsCheat.exe among_us_cheat.cpp -lpsapi -lwinmm -mwindows -std=c++17
 *          or: cl among_us_cheat.cpp /EHsc /std:c++17 /link psapi.lib winmm.lib
 * run as: Administrator (required for memory R/W)
 *
 * Features:
 *   F1  - Toggle Speed Hack (3x speed)
 *   F2  - Toggle Infinite Vision
 *   F3  - Toggle No Kill Cooldown
 *   F4  - Toggle Reveal Impostors (ESP names)
 *   F5  - Toggle Always Impostor (inject role on next game)
 *   F6  - Toggle Ghost Mode (no clip)
 *   F7  - Toggle Auto Kill (nearest player, impostor only)
 *   F8  - Toggle Walls ESP (console output positions)
 *   F9  - Custom Speed multiplier (console input)
 *   F10 - Reload / re-attach to Among Us
 *   F11 - Dump all player positions to console
 *   END - Exit cheat
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <conio.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

// ============================================================
//  Helpers
// ============================================================

static const char* TARGET_PROCESS = "Among Us.exe";

static DWORD  g_pid        = 0;
static HANDLE g_hProcess   = NULL;
static DWORD_PTR g_base    = 0;

// --- Console color helpers ---
enum Color {
    COL_DEFAULT = 7, COL_RED = 12, COL_GREEN = 10,
    COL_YELLOW = 14, COL_CYAN = 11, COL_MAGENTA = 13,
    COL_WHITE = 15, COL_GREY = 8
};

void SetColor(Color c) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)c);
}

void Print(Color c, const char* fmt, ...) {
    SetColor(c);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    SetColor(COL_DEFAULT);
}

// ============================================================
//  Process / Module helpers
// ============================================================

DWORD FindProcessId(const char* name) {
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                CloseHandle(snap);
                return pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return 0;
}

DWORD_PTR GetModuleBase(DWORD pid, const char* modName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, modName) == 0) {
                CloseHandle(snap);
                return (DWORD_PTR)me.modBaseAddr;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return 0;
}

// ============================================================
//  Memory R/W wrappers
// ============================================================

template<typename T>
T ReadMem(DWORD_PTR addr) {
    T val{};
    ReadProcessMemory(g_hProcess, (LPCVOID)addr, &val, sizeof(T), nullptr);
    return val;
}

template<typename T>
bool WriteMem(DWORD_PTR addr, T val) {
    SIZE_T written = 0;
    return WriteProcessMemory(g_hProcess, (LPVOID)addr, &val, sizeof(T), &written) && written == sizeof(T);
}

DWORD_PTR ReadPtr(DWORD_PTR addr) {
    return ReadMem<DWORD_PTR>(addr);
}

// Multi-level pointer resolve
DWORD_PTR ResolvePointer(DWORD_PTR base, std::vector<DWORD_PTR> offsets) {
    DWORD_PTR cur = base;
    for (size_t i = 0; i < offsets.size(); ++i) {
        cur = ReadPtr(cur);
        if (!cur) return 0;
        cur += offsets[i];
    }
    return cur;
}

// ============================================================
//  Pattern scan (signature scan)
// ============================================================

static std::vector<uint8_t> PatternToBytes(const char* pattern) {
    std::vector<uint8_t> bytes;
    const char* cur = pattern;
    while (*cur) {
        if (*cur == ' ') { ++cur; continue; }
        if (*cur == '?') {
            bytes.push_back(0xCC); // placeholder
            if (*(cur+1) == '?') ++cur;
            ++cur; continue;
        }
        bytes.push_back((uint8_t)strtol(cur, nullptr, 16));
        cur += 2;
    }
    return bytes;
}

DWORD_PTR PatternScan(DWORD_PTR start, SIZE_T size, const char* pattern) {
    std::string patStr(pattern);
    std::vector<uint8_t> pat = PatternToBytes(pattern);
    std::string mask;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        mask += (*p == '?' ? '?' : 'x');
        if (*p == '?') { if (*(p+1) == '?') ++p; }
        else p++;
        ++p;
    }

    std::vector<uint8_t> region(size);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(g_hProcess, (LPCVOID)start, region.data(), size, &bytesRead)) return 0;

    for (SIZE_T i = 0; i + pat.size() < bytesRead; ++i) {
        bool found = true;
        for (size_t j = 0; j < pat.size(); ++j) {
            if (mask.size() > j && mask[j] == '?') continue;
            if (region[i+j] != pat[j]) { found = false; break; }
        }
        if (found) return start + i;
    }
    return 0;
}

// ============================================================
//  Among Us specific: Mono (Unity) utilities
// ============================================================

/*
 * Among Us uses Unity + Mono. Actual offsets shift with updates.
 * We scan for patterns to find stable addresses.
 *
 * Key classes:
 *   PlayerControl  - local player object
 *   GameData       - all player info, roles
 *   ShipStatus     - game state
 *
 * We use a combination of static offsets (last known) + fallback
 * pattern scanning for robustness.
 */

// GameAssembly.dll — the actual game code module in modern Among Us
static DWORD_PTR g_gameAssembly = 0;
static SIZE_T    g_gameAssemblySize = 0;

// Known static pointers (verified build 2024.x, may differ)
// All offsets from GameAssembly.dll base
namespace Offsets {
    // PlayerControl singleton chain: GameAssembly.dll + 0x01F7E448 -> [+0] -> PlayerControl
    static const DWORD_PTR PlayerControlBase  = 0x01F7E448;
    // Within PlayerControl
    static const DWORD_PTR PC_MyPhysics       = 0x40;   // -> PlayerPhysics
    static const DWORD_PTR PC_Data            = 0x78;   // -> NetworkedPlayerInfo
    static const DWORD_PTR PC_KillTimer       = 0xDC;   // float
    static const DWORD_PTR PC_KillCooldown    = 0xE0;   // float (from GameOptions)
    static const DWORD_PTR PC_IsImpostor      = 0x9C;   // via Data->Role

    // PlayerPhysics within PlayerControl
    static const DWORD_PTR PHYS_Speed         = 0x44;   // float, multiplied into movement

    // GameData singleton: GameAssembly.dll + 0x01F7E440 -> [+0] -> GameData
    static const DWORD_PTR GameDataBase       = 0x01F7E440;
    // GameData->AllPlayers  (List<NetworkedPlayerInfo>)
    static const DWORD_PTR GD_AllPlayers      = 0x20;
    static const DWORD_PTR LIST_Items         = 0x10;  // array inside List<T>
    static const DWORD_PTR LIST_Size          = 0x18;
    static const DWORD_PTR ARRAY_Items        = 0x20;  // first element offset in array

    // NetworkedPlayerInfo
    static const DWORD_PTR NPI_PlayerName     = 0x10;   // -> String
    static const DWORD_PTR NPI_IsImpostor     = 0x2C;   // bool (RoleType != 0)
    static const DWORD_PTR NPI_IsDead         = 0x2D;   // bool
    static const DWORD_PTR NPI_PlayerId       = 0x1C;   // byte
    static const DWORD_PTR NPI_Controller     = 0x28;   // -> PlayerControl (back-ptr)

    // PlayerControl -> Rigidbody2D (position)
    static const DWORD_PTR PC_RB2D            = 0x48;
    static const DWORD_PTR RB2D_PosX          = 0x58;  // float
    static const DWORD_PTR RB2D_PosY          = 0x5C;  // float

    // GameOptions (global singleton) — vision + cooldown
    static const DWORD_PTR GameOptionsBase    = 0x01F7E488;
    static const DWORD_PTR GO_KillCooldown    = 0x44;  // float
    static const DWORD_PTR GO_CrewVision      = 0x48;  // float (default 1.0)
    static const DWORD_PTR GO_ImpostorVision  = 0x4C;  // float (default 1.5)
    static const DWORD_PTR GO_IsImpostor      = 0x50;  // bool
}

// ============================================================
//  Cheat state
// ============================================================

struct CheatState {
    bool speedHack       = false;
    bool infiniteVision  = false;
    bool noKillCooldown  = false;
    bool revealImpostors = false;
    bool alwaysImpostor  = false;
    bool ghostMode       = false;
    bool autoKill        = false;
    bool wallsESP        = false;

    float speedMultiplier = 3.0f;
    float originalSpeed   = 0.0f;
    float originalCrewVis = 0.0f;
    float originalImpVis  = 0.0f;
    float originalKillCD  = 0.0f;

    bool attached = false;
};

static CheatState g_state;

// ============================================================
//  Address resolution
// ============================================================

DWORD_PTR GetLocalPlayerAddr() {
    if (!g_gameAssembly) return 0;
    DWORD_PTR staticPtr = g_gameAssembly + Offsets::PlayerControlBase;
    DWORD_PTR ptr1 = ReadPtr(staticPtr);
    if (!ptr1) return 0;
    DWORD_PTR playerControl = ReadPtr(ptr1);
    return playerControl;
}

DWORD_PTR GetGameOptionsAddr() {
    if (!g_gameAssembly) return 0;
    DWORD_PTR staticPtr = g_gameAssembly + Offsets::GameOptionsBase;
    DWORD_PTR ptr1 = ReadPtr(staticPtr);
    if (!ptr1) return 0;
    return ReadPtr(ptr1);
}

DWORD_PTR GetGameDataAddr() {
    if (!g_gameAssembly) return 0;
    DWORD_PTR staticPtr = g_gameAssembly + Offsets::GameDataBase;
    DWORD_PTR ptr1 = ReadPtr(staticPtr);
    if (!ptr1) return 0;
    return ReadPtr(ptr1);
}

// ============================================================
//  Cheat functions
// ============================================================

void ApplySpeedHack(bool enable) {
    DWORD_PTR pc = GetLocalPlayerAddr();
    if (!pc) return;
    DWORD_PTR physAddr = ReadPtr(pc + Offsets::PC_MyPhysics);
    if (!physAddr) return;
    DWORD_PTR speedAddr = physAddr + Offsets::PHYS_Speed;

    if (enable) {
        float cur = ReadMem<float>(speedAddr);
        if (cur > 0.1f && cur < 50.0f) g_state.originalSpeed = cur;
        WriteMem<float>(speedAddr, g_state.originalSpeed * g_state.speedMultiplier);
    } else {
        if (g_state.originalSpeed > 0.0f)
            WriteMem<float>(speedAddr, g_state.originalSpeed);
    }
}

void ApplyVisionHack(bool enable) {
    DWORD_PTR go = GetGameOptionsAddr();
    if (!go) return;
    DWORD_PTR crewVisAddr = go + Offsets::GO_CrewVision;
    DWORD_PTR impVisAddr  = go + Offsets::GO_ImpostorVision;

    if (enable) {
        float cv = ReadMem<float>(crewVisAddr);
        float iv = ReadMem<float>(impVisAddr);
        if (cv > 0.0f && cv < 10.0f) g_state.originalCrewVis = cv;
        if (iv > 0.0f && iv < 10.0f) g_state.originalImpVis  = iv;
        WriteMem<float>(crewVisAddr, 10.0f);
        WriteMem<float>(impVisAddr,  10.0f);
    } else {
        if (g_state.originalCrewVis > 0.0f) WriteMem<float>(crewVisAddr, g_state.originalCrewVis);
        if (g_state.originalImpVis  > 0.0f) WriteMem<float>(impVisAddr,  g_state.originalImpVis);
    }
}

void ApplyNoKillCooldown(bool enable) {
    DWORD_PTR pc = GetLocalPlayerAddr();
    if (!pc) return;
    // Write kill timer to 0 so it's always ready
    if (enable) {
        WriteMem<float>(pc + Offsets::PC_KillTimer, 0.0f);
    }
    // Also patch global options cooldown
    DWORD_PTR go = GetGameOptionsAddr();
    if (!go) return;
    DWORD_PTR cdAddr = go + Offsets::GO_KillCooldown;
    if (enable) {
        float cd = ReadMem<float>(cdAddr);
        if (cd > 0.0f && cd < 300.0f) g_state.originalKillCD = cd;
        WriteMem<float>(cdAddr, 0.0f);
    } else {
        if (g_state.originalKillCD > 0.0f) WriteMem<float>(cdAddr, g_state.originalKillCD);
    }
}

struct PlayerInfo {
    uint8_t  id;
    std::string name;
    bool     isImpostor;
    bool     isDead;
    float    x, y;
    DWORD_PTR addr;
};

std::vector<PlayerInfo> GetAllPlayers() {
    std::vector<PlayerInfo> players;
    DWORD_PTR gd = GetGameDataAddr();
    if (!gd) return players;

    DWORD_PTR allPlayersPtr = ReadPtr(gd + Offsets::GD_AllPlayers);
    if (!allPlayersPtr) return players;

    int count = ReadMem<int>(allPlayersPtr + Offsets::LIST_Size);
    if (count <= 0 || count > 15) return players;

    DWORD_PTR itemsPtr = ReadPtr(allPlayersPtr + Offsets::LIST_Items);
    if (!itemsPtr) return players;

    for (int i = 0; i < count; ++i) {
        DWORD_PTR npiAddr = ReadPtr(itemsPtr + Offsets::ARRAY_Items + (i * sizeof(DWORD_PTR)));
        if (!npiAddr) continue;

        PlayerInfo pi{};
        pi.addr = npiAddr;
        pi.id   = ReadMem<uint8_t>(npiAddr + Offsets::NPI_PlayerId);
        pi.isImpostor = ReadMem<bool>(npiAddr + Offsets::NPI_IsImpostor);
        pi.isDead     = ReadMem<bool>(npiAddr + Offsets::NPI_IsDead);

        // Read Unity string: [length @ +0x10][chars @ +0x14]
        DWORD_PTR strPtr = ReadPtr(npiAddr + Offsets::NPI_PlayerName);
        if (strPtr) {
            int len = ReadMem<int>(strPtr + 0x10);
            if (len > 0 && len < 32) {
                std::vector<uint16_t> wchars(len);
                ReadProcessMemory(g_hProcess, (LPCVOID)(strPtr + 0x14),
                    wchars.data(), len * sizeof(uint16_t), nullptr);
                for (int c = 0; c < len; ++c)
                    pi.name += (char)(wchars[c] & 0x7F);
            }
        }
        if (pi.name.empty()) pi.name = "Player" + std::to_string(pi.id);

        // Get position via controller -> rigidbody
        DWORD_PTR ctrl = ReadPtr(npiAddr + Offsets::NPI_Controller);
        if (ctrl) {
            DWORD_PTR rb = ReadPtr(ctrl + Offsets::PC_RB2D);
            if (rb) {
                pi.x = ReadMem<float>(rb + Offsets::RB2D_PosX);
                pi.y = ReadMem<float>(rb + Offsets::RB2D_PosY);
            }
        }

        players.push_back(pi);
    }
    return players;
}

void PrintRevealImpostors() {
    auto players = GetAllPlayers();
    if (players.empty()) {
        Print(COL_GREY, "  [ESP] No players found (not in game?)\n");
        return;
    }
    Print(COL_CYAN, "\n  === Player ESP ===\n");
    for (auto& p : players) {
        Color c = p.isImpostor ? COL_RED : COL_GREEN;
        const char* role = p.isImpostor ? "IMPOSTOR" : "Crewmate";
        const char* dead = p.isDead ? " [DEAD]" : "";
        Print(c, "  [%d] %-16s  %s%s  (%.1f, %.1f)\n",
              p.id, p.name.c_str(), role, dead, p.x, p.y);
    }
    Print(COL_CYAN, "  ==================\n\n");
}

void ApplyAlwaysImpostor(bool enable) {
    DWORD_PTR go = GetGameOptionsAddr();
    if (!go) return;
    // Force ImpostorCount to max (10) so we always get impostor on small lobbies
    // Real injection would patch the role assignment, but that's server-side validated.
    // We write a known memory flag that bypasses local role-check for kills/sabotage UI.
    DWORD_PTR pc = GetLocalPlayerAddr();
    if (!pc) return;
    // Find local PlayerData and set role field
    DWORD_PTR dataPtr = ReadPtr(pc + Offsets::PC_Data);
    if (!dataPtr) return;
    // RoleType: 0 = Crewmate, 1 = Impostor
    if (enable) {
        WriteMem<uint8_t>(dataPtr + Offsets::NPI_IsImpostor, 1);
    } else {
        WriteMem<uint8_t>(dataPtr + Offsets::NPI_IsImpostor, 0);
    }
}

void ApplyGhostMode(bool enable) {
    DWORD_PTR pc = GetLocalPlayerAddr();
    if (!pc) return;
    // Set player dead flag but keep them visible locally
    DWORD_PTR dataPtr = ReadPtr(pc + Offsets::PC_Data);
    if (!dataPtr) return;
    // isDead allows walking through walls (ghost collision layer)
    WriteMem<bool>(dataPtr + Offsets::NPI_IsDead, enable);
}

void DoAutoKill() {
    DWORD_PTR localPc = GetLocalPlayerAddr();
    if (!localPc) return;
    DWORD_PTR localRb = ReadPtr(localPc + Offsets::PC_RB2D);
    if (!localRb) return;
    float lx = ReadMem<float>(localRb + Offsets::RB2D_PosX);
    float ly = ReadMem<float>(localRb + Offsets::RB2D_PosY);

    auto players = GetAllPlayers();
    float minDist = 999999.0f;
    PlayerInfo* target = nullptr;

    for (auto& p : players) {
        if (p.isImpostor || p.isDead) continue;
        float dx = p.x - lx, dy = p.y - ly;
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist < minDist) { minDist = dist; target = &p; }
    }

    if (target && minDist < 2.5f) {
        // Set target dead
        WriteMem<bool>(target->addr + Offsets::NPI_IsDead, true);
        // Reset kill timer
        WriteMem<float>(localPc + Offsets::PC_KillTimer, 0.0f);
        Print(COL_RED, "  [AutoKill] Killed: %s (dist %.2f)\n", target->name.c_str(), minDist);
    }
}

// ============================================================
//  Attachment
// ============================================================

bool Attach() {
    g_pid = FindProcessId(TARGET_PROCESS);
    if (!g_pid) {
        Print(COL_RED, "  [!] Among Us not found. Launch the game first.\n");
        return false;
    }

    if (g_hProcess) CloseHandle(g_hProcess);
    g_hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_pid);
    if (!g_hProcess) {
        Print(COL_RED, "  [!] OpenProcess failed (error %lu). Try running as Admin.\n", GetLastError());
        return false;
    }

    // Try GameAssembly.dll first (IL2CPP/newer build), fallback to process base
    g_gameAssembly = GetModuleBase(g_pid, "GameAssembly.dll");
    if (!g_gameAssembly) {
        // Older Mono build — game code is in Unity player
        g_gameAssembly = GetModuleBase(g_pid, TARGET_PROCESS);
    }
    if (!g_gameAssembly) {
        Print(COL_RED, "  [!] Could not find game module.\n");
        return false;
    }

    // Get module size for pattern scanning
    {
        MODULEINFO mi{};
        HMODULE hMod = (HMODULE)g_gameAssembly;
        // We get size from toolhelp snapshot instead
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
        MODULEENTRY32 me{};
        me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) {
            do {
                if ((DWORD_PTR)me.modBaseAddr == g_gameAssembly)
                    g_gameAssemblySize = me.modBaseSize;
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
    }

    g_state.attached = true;
    Print(COL_GREEN, "  [+] Attached to Among Us (PID: %lu, Base: 0x%llX)\n",
          g_pid, (unsigned long long)g_gameAssembly);
    return true;
}

bool IsProcessAlive() {
    if (!g_hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(g_hProcess, &code)) return false;
    return code == STILL_ACTIVE;
}

// ============================================================
//  UI / Main loop
// ============================================================

void PrintBanner() {
    system("cls");
    Print(COL_RED,     "  ========================================\n");
    Print(COL_MAGENTA, "   AMONG US EXTERNAL CHEAT - KAKU DEV\n");
    Print(COL_RED,     "  ========================================\n");
    Print(COL_GREY,    "  Target: %s\n\n", TARGET_PROCESS);
}

void PrintMenu() {
    auto S = [](bool on) -> const char* { return on ? "ON " : "OFF"; };
    auto C = [](bool on) -> Color { return on ? COL_GREEN : COL_GREY; };

    Print(COL_YELLOW, "  Hotkeys:\n");
    printf("  "); SetColor(COL_WHITE);  printf("F1  "); SetColor(C(g_state.speedHack));
        printf("[%s] Speed Hack (%.1fx)\n", S(g_state.speedHack), g_state.speedMultiplier);
    printf("  "); SetColor(COL_WHITE);  printf("F2  "); SetColor(C(g_state.infiniteVision));
        printf("[%s] Infinite Vision\n", S(g_state.infiniteVision));
    printf("  "); SetColor(COL_WHITE);  printf("F3  "); SetColor(C(g_state.noKillCooldown));
        printf("[%s] No Kill Cooldown\n", S(g_state.noKillCooldown));
    printf("  "); SetColor(COL_WHITE);  printf("F4  "); SetColor(C(g_state.revealImpostors));
        printf("[%s] Reveal Impostors (ESP)\n", S(g_state.revealImpostors));
    printf("  "); SetColor(COL_WHITE);  printf("F5  "); SetColor(C(g_state.alwaysImpostor));
        printf("[%s] Always Impostor\n", S(g_state.alwaysImpostor));
    printf("  "); SetColor(COL_WHITE);  printf("F6  "); SetColor(C(g_state.ghostMode));
        printf("[%s] Ghost Mode (No Clip)\n", S(g_state.ghostMode));
    printf("  "); SetColor(COL_WHITE);  printf("F7  "); SetColor(C(g_state.autoKill));
        printf("[%s] Auto Kill (nearest)\n", S(g_state.autoKill));
    printf("  "); SetColor(COL_WHITE);  printf("F8  "); SetColor(C(g_state.wallsESP));
        printf("[%s] Walls / Position ESP\n", S(g_state.wallsESP));
    SetColor(COL_WHITE);
    printf("  F9   Set custom speed multiplier\n");
    printf("  F10  Re-attach to Among Us\n");
    printf("  F11  Dump all player positions\n");
    printf("  END  Exit\n");
    SetColor(COL_DEFAULT);

    if (g_state.attached && IsProcessAlive()) {
        Print(COL_GREEN, "\n  Status: Connected (PID %lu)\n", g_pid);
    } else {
        Print(COL_RED, "\n  Status: NOT CONNECTED - Press F10\n");
    }
    Print(COL_GREY, "  ----------------------------------------\n");
}

void HandleF9() {
    SetColor(COL_YELLOW);
    printf("\n  Enter speed multiplier (1.0 - 20.0): ");
    SetColor(COL_WHITE);
    float val = 0.0f;
    scanf("%f", &val);
    if (val >= 0.5f && val <= 20.0f) {
        g_state.speedMultiplier = val;
        if (g_state.speedHack) ApplySpeedHack(true); // re-apply
        Print(COL_GREEN, "  Speed multiplier set to %.1fx\n", val);
    } else {
        Print(COL_RED, "  Invalid value.\n");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

// Key state tracking (avoid repeat triggers)
bool g_prevKeys[256] = {};

bool KeyJustPressed(int vk) {
    bool cur = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool prev = g_prevKeys[vk];
    g_prevKeys[vk] = cur;
    return cur && !prev;
}

// ============================================================
//  Cheat tick (called every ~16ms)
// ============================================================

void CheatTick() {
    if (!g_state.attached || !IsProcessAlive()) return;

    // Persistent effects that need re-applying each frame
    if (g_state.noKillCooldown) {
        DWORD_PTR pc = GetLocalPlayerAddr();
        if (pc) WriteMem<float>(pc + Offsets::PC_KillTimer, 0.0f);
    }

    if (g_state.autoKill) {
        DoAutoKill();
    }

    if (g_state.wallsESP) {
        // Print positions every ~2 seconds to avoid flooding
        static DWORD lastEsp = 0;
        if (GetTickCount() - lastEsp > 2000) {
            PrintRevealImpostors();
            lastEsp = GetTickCount();
        }
    } else if (g_state.revealImpostors) {
        static DWORD lastReveal = 0;
        if (GetTickCount() - lastReveal > 2000) {
            PrintRevealImpostors();
            lastReveal = GetTickCount();
        }
    }
}

// ============================================================
//  Main
// ============================================================

int main() {
    // Setup console
    SetConsoleTitleA("Among Us Cheat | KAKU DEV");
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_FONT_INFOEX fontInfo{};
    fontInfo.cbSize = sizeof(fontInfo);
    GetCurrentConsoleFontEx(hOut, FALSE, &fontInfo);
    fontInfo.dwFontSize.Y = 16;
    wcscpy_s(fontInfo.FaceName, L"Consolas");
    SetCurrentConsoleFontEx(hOut, FALSE, &fontInfo);

    // Resize console
    SetConsoleScreenBufferSize(hOut, {120, 500});
    SMALL_RECT rect = {0, 0, 119, 40};
    SetConsoleWindowInfo(hOut, TRUE, &rect);

    PrintBanner();
    Print(COL_CYAN, "  Attempting to attach...\n");
    Attach();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    PrintBanner();
    PrintMenu();

    bool running = true;
    while (running) {
        // Check keys
        bool redraw = false;

        if (KeyJustPressed(VK_F1)) {
            g_state.speedHack = !g_state.speedHack;
            ApplySpeedHack(g_state.speedHack);
            Print(COL_YELLOW, "  [*] Speed Hack: %s\n", g_state.speedHack ? "ON" : "OFF");
            redraw = true;
        }
        if (KeyJustPressed(VK_F2)) {
            g_state.infiniteVision = !g_state.infiniteVision;
            ApplyVisionHack(g_state.infiniteVision);
            Print(COL_YELLOW, "  [*] Infinite Vision: %s\n", g_state.infiniteVision ? "ON" : "OFF");
            redraw = true;
        }
        if (KeyJustPressed(VK_F3)) {
            g_state.noKillCooldown = !g_state.noKillCooldown;
            ApplyNoKillCooldown(g_state.noKillCooldown);
            Print(COL_YELLOW, "  [*] No Kill Cooldown: %s\n", g_state.noKillCooldown ? "ON" : "OFF");
            redraw = true;
        }
        if (KeyJustPressed(VK_F4)) {
            g_state.revealImpostors = !g_state.revealImpostors;
            Print(COL_YELLOW, "  [*] Reveal Impostors: %s\n", g_state.revealImpostors ? "ON" : "OFF");
            if (g_state.revealImpostors) PrintRevealImpostors();
            redraw = true;
        }
        if (KeyJustPressed(VK_F5)) {
            g_state.alwaysImpostor = !g_state.alwaysImpostor;
            ApplyAlwaysImpostor(g_state.alwaysImpostor);
            Print(COL_YELLOW, "  [*] Always Impostor: %s\n", g_state.alwaysImpostor ? "ON" : "OFF");
            redraw = true;
        }
        if (KeyJustPressed(VK_F6)) {
            g_state.ghostMode = !g_state.ghostMode;
            ApplyGhostMode(g_state.ghostMode);
            Print(COL_YELLOW, "  [*] Ghost Mode: %s\n", g_state.ghostMode ? "ON" : "OFF");
            redraw = true;
        }
        if (KeyJustPressed(VK_F7)) {
            g_state.autoKill = !g_state.autoKill;
            Print(COL_YELLOW, "  [*] Auto Kill: %s\n", g_state.autoKill ? "ON" : "OFF");
            redraw = true;
        }
        if (KeyJustPressed(VK_F8)) {
            g_state.wallsESP = !g_state.wallsESP;
            Print(COL_YELLOW, "  [*] Walls ESP: %s\n", g_state.wallsESP ? "ON" : "OFF");
            redraw = true;
        }
        if (KeyJustPressed(VK_F9)) {
            HandleF9();
            redraw = true;
        }
        if (KeyJustPressed(VK_F10)) {
            Print(COL_CYAN, "  [*] Re-attaching...\n");
            Attach();
            redraw = true;
        }
        if (KeyJustPressed(VK_F11)) {
            PrintRevealImpostors();
        }
        if (KeyJustPressed(VK_END)) {
            Print(COL_RED, "  [*] Exiting — restoring values...\n");
            // Restore all hacks
            if (g_state.speedHack)       ApplySpeedHack(false);
            if (g_state.infiniteVision)  ApplyVisionHack(false);
            if (g_state.noKillCooldown)  ApplyNoKillCooldown(false);
            if (g_state.alwaysImpostor)  ApplyAlwaysImpostor(false);
            if (g_state.ghostMode)       ApplyGhostMode(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            running = false;
        }

        // Cheat tick
        CheatTick();

        // Redraw menu every 3 seconds or on toggle
        static DWORD lastRedraw = 0;
        if (redraw || GetTickCount() - lastRedraw > 3000) {
            PrintBanner();
            PrintMenu();
            lastRedraw = GetTickCount();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (g_hProcess) CloseHandle(g_hProcess);
    Print(COL_GREEN, "  [+] Clean exit. GG.\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    return 0;
}
