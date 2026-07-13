// ═══════════════════════════════════════════════════════════════════════
//  SHARED TYPES & IOCTL DEFINITIONS
//  Kernel ↔ User-Mode Communication Protocol
// ═══════════════════════════════════════════════════════════════════════

#ifndef SHARED_TYPES_H
#define SHARED_TYPES_H

#ifdef _KERNEL_MODE
#include <ntdef.h>
#else
#include <Windows.h>
#endif

#pragma pack(push, 1)

// ───────────────────────────────────────────────────────────────────────
//  MATH STRUCTURES (match user-mode definitions)
// ───────────────────────────────────────────────────────────────────────
typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    float x, y;
} Vec2;

// ───────────────────────────────────────────────────────────────────────
//  IOCTL CODES
// ───────────────────────────────────────────────────────────────────────
#define FILE_DEVICE_BLINDEYE 0x8888
#define IOCTL_BLINDEYE_READ_MEMORY   CTL_CODE(FILE_DEVICE_BLINDEYE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BLINDEYE_QUERY_ENTITY  CTL_CODE(FILE_DEVICE_BLINDEYE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BLINDEYE_GET_GAME_BASE CTL_CODE(FILE_DEVICE_BLINDEYE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BLINDEYE_SCAN_OFFSETS  CTL_CODE(FILE_DEVICE_BLINDEYE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ───────────────────────────────────────────────────────────────────────
//  REQUEST/RESPONSE STRUCTURES
// ───────────────────────────────────────────────────────────────────────

// Generic memory read
typedef struct {
    ULONG64 Address;
    ULONG   Size;
    ULONG   Reserved;
} BLINDEYE_READ_REQUEST;

typedef struct {
    UCHAR Data[4096];
    ULONG BytesRead;
} BLINDEYE_READ_RESPONSE;

// Entity query
typedef enum {
    FACTION_UNKNOWN = 0,
    FACTION_BLUFOR  = 1,
    FACTION_OPFOR   = 2,
    FACTION_INDFOR  = 3,
} ENTITY_FACTION;

typedef enum {
    LIFE_ALIVE = 0,
    LIFE_INCAP = 1,
    LIFE_DEAD  = 2,
} ENTITY_LIFE_STATE;

typedef struct {
    ULONG64      EntityPtr;
    Vec3         WorldPos;
    Vec3         HeadPos;
    float        CurHP;
    float        MaxHP;
    ENTITY_LIFE_STATE LifeState;
    ENTITY_FACTION    Faction;
    char         Name[64];
    ULONG        NameLen;
} BLINDEYE_ENTITY;

typedef struct {
    ULONG64 GameBaseAddress;
    ULONG64 WorldPtr;
    ULONG64 EntityListPtr;
    ULONG   EntityCount;
} BLINDEYE_GAME_STATE;

// Get game base
typedef struct {
    ULONG ProcessId;
    ULONG Reserved;
} BLINDEYE_GET_BASE_REQUEST;

typedef struct {
    ULONG64 GameBase;
    ULONG64 GameSize;
    ULONG   Status;
} BLINDEYE_GET_BASE_RESPONSE;

// Scan/cache offsets
typedef struct {
    ULONG64 GameBase;
} BLINDEYE_SCAN_OFFSETS_REQUEST;

typedef struct {
    ULONG64 GamePtr;
    ULONG64 World;
    ULONG64 PlayerMgr;
    ULONG64 EntityList;
    ULONG   EntityCount;
    ULONG64 LocalCtrlWeak;
    ULONG64 CamMgrWeak;
    ULONG64 Reserved[8];
    ULONG   Status;
} BLINDEYE_SCAN_OFFSETS_RESPONSE;

#pragma pack(pop)

#endif // SHARED_TYPES_H
