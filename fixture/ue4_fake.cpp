// ue4_fake — a synthetic UE4 process for testing dumpers.
//
// Builds, in its own address space, the three structures every UE4 dumper
// walks — FNamePool, FUObjectArray and a UWorld chain — laid out at the
// offsets stock UE 4.25/4.27 arm64 uses. Then it prints the ground truth
// (global offsets from module base, the real struct offsets, and a sample of
// index->name pairs) and optionally holds so an external dumper can attach.
//
// The point: a dumper run against this has a known-correct answer, so any
// disagreement is a bug in the dumper, not in the target.
//
//   ./ue4_fake --selftest        walk our own structures, print what a
//                                correct dumper should produce
//   ./ue4_fake --hold 600        print truth, then sleep 600s for attach
//   ./ue4_fake --json out.json   write ground truth as JSON
//
// Build: see fixture/build.sh (NDK clang++, aarch64, static libc++).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unistd.h>
#include <dlfcn.h>
#include "../common/ue4emit.h"

// ---------------------------------------------------------------------------
// Ground-truth layout. These are the offsets a correct dumper must use against
// this binary. They mirror stock UE 4.25/4.27 for arm64 (LP64).
// ---------------------------------------------------------------------------
namespace Truth {
    // FNamePool (4.23+)
    constexpr size_t FNamePoolToCurrentBlock      = 0x08;
    constexpr size_t FNamePoolToCurrentByteCursor = 0x0C;
    constexpr size_t FNamePoolToBlocks            = 0x10;
    constexpr size_t FNameStride                  = 0x02;
    constexpr size_t FNameEntryToString           = 0x02;  // sizeof(header)
    constexpr size_t FNameEntryToLenBit           = 6;
    constexpr size_t BlockSizeBytes               = FNameStride * 65536;

    // FUObjectArray -> FChunkedFixedUObjectArray
    constexpr size_t FUObjectArrayToTUObjectArray = 0x10;
    constexpr size_t TUObjectArrayToObjects       = 0x00;  // from TUObjectArray
    constexpr size_t TUObjectArrayToMaxElements   = 0x10;
    constexpr size_t TUObjectArrayToNumElements   = 0x14;  // NB: dumper default is 0xC
    constexpr size_t FUObjectItemSize             = 0x18;
    constexpr size_t FUObjectItemPadd             = 0x00;
    constexpr size_t ObjectsPerChunk              = 0x10000;

    // UObject
    constexpr size_t UObjectToVTable        = 0x00;
    constexpr size_t UObjectToObjectFlags   = 0x08;
    constexpr size_t UObjectToInternalIndex = 0x0C;
    constexpr size_t UObjectToClassPrivate  = 0x10;
    constexpr size_t UObjectToFNameIndex    = 0x18;
    constexpr size_t UObjectToOuterPrivate  = 0x20;
    constexpr size_t SizeOfUObject          = 0x28;

    // UField : UObject
    constexpr size_t UFieldToNext = 0x28;
    constexpr size_t SizeOfUField = 0x30;

    // UStruct : UField
    constexpr size_t UStructToSuperStruct    = 0x30;
    constexpr size_t UStructToChildren       = 0x38;
    // ChildProperties takes 0x40 in 4.25+, which is why PropertiesSize sits
    // at 0x48 and not 0x40 as it did before.
    constexpr size_t UStructToPropertiesSize = 0x48;
    constexpr size_t SizeOfUStruct           = 0x58;

    // UProperty : UField
    constexpr size_t UPropertyToArrayDim       = 0x30;  // NB: dumper hardcodes 0x34
    constexpr size_t UPropertyToElementSize    = 0x34;
    constexpr size_t UPropertyToPropertyFlags  = 0x38;
    constexpr size_t UPropertyToOffsetInternal = 0x44;
    constexpr size_t SizeOfUProperty           = 0x50;

    // FField / FProperty — the 4.25+ layout. Properties left the UObject
    // system: they are no longer in GUObjectArray and no longer on Children.
    constexpr size_t UStructToChildProperties   = 0x40;
    constexpr size_t FFieldToClass              = 0x08;
    constexpr size_t FFieldToNext               = 0x20;
    constexpr size_t FFieldToName               = 0x28;
    constexpr size_t FPropertyToArrayDim        = 0x38;
    constexpr size_t FPropertyToElementSize     = 0x3C;
    constexpr size_t FPropertyToOffsetInternal  = 0x4C;
    constexpr size_t SizeOfFProperty            = 0x78;
    constexpr size_t SizeOfFFieldClass          = 0x20;

    // UWorld : UObject / ULevel : UObject
    constexpr size_t UWorldToPersistentLevel = 0x30;
    constexpr size_t ULevelToAActors         = 0x98;  // TArray::Data
    constexpr size_t ULevelToAActorsCount    = 0xA0;  // TArray::Num
}

// ---------------------------------------------------------------------------
// Raw field writers. Everything is built by offset, never by C++ struct, so
// the layout is exactly what we document above and not what a compiler chose.
// ---------------------------------------------------------------------------
static inline void W8 (void* b, size_t off, uint8_t  v) { memcpy((char*)b + off, &v, 1); }
static inline void W32(void* b, size_t off, uint32_t v) { memcpy((char*)b + off, &v, 4); }
static inline void W64(void* b, size_t off, uint64_t v) { memcpy((char*)b + off, &v, 8); }
static inline uint16_t R16(const void* b, size_t off) { uint16_t v; memcpy(&v, (const char*)b + off, 2); return v; }
static inline uint32_t R32(const void* b, size_t off) { uint32_t v; memcpy(&v, (const char*)b + off, 4); return v; }
static inline uint64_t R64(const void* b, size_t off) { uint64_t v; memcpy(&v, (const char*)b + off, 8); return v; }

// ---------------------------------------------------------------------------
// Globals. Declared at file scope so they live in .bss and therefore have
// stable module-relative offsets — exactly like the real GNames/GUObjectArray/
// GWorld. The pool and the object array are inline (not pointers), matching
// stock UE4; GWorld is a pointer.
// ---------------------------------------------------------------------------
struct FNamePoolStorage {
    uint8_t  Lock[8];
    uint32_t CurrentBlock;
    uint32_t CurrentByteCursor;
    uint8_t* Blocks[8192];
};

static FNamePoolStorage GNamesPool;     // <- "GNames"
static uint8_t          GUObjectArray[0x30];
static void*            GWorld = nullptr;

// A fake vtable pointer so objects look plausible to a validity check.
static void* FakeVTable[8];

// ---------------------------------------------------------------------------
// Name pool
// ---------------------------------------------------------------------------
struct NameTable {
    std::vector<std::string> names;      // index in this vector is NOT the FName id
    std::vector<uint32_t>    ids;        // ids[i] is the FName id of names[i]

    uint32_t add(const std::string& s) {
        uint32_t block  = GNamesPool.CurrentBlock;
        uint32_t cursor = GNamesPool.CurrentByteCursor;

        // header (2) + chars, rounded up to FNameStride
        size_t need = Truth::FNameEntryToString + s.size();
        need = (need + Truth::FNameStride - 1) & ~(Truth::FNameStride - 1);

        if (cursor + need + Truth::FNameStride > Truth::BlockSizeBytes) {
            block += 1;
            cursor = 0;
            GNamesPool.CurrentBlock = block;
            GNamesPool.Blocks[block] = (uint8_t*)calloc(1, Truth::BlockSizeBytes);
        }

        uint8_t* entry = GNamesPool.Blocks[block] + cursor;

        // FNameEntryHeader: bit 0 = bIsWide, bits 6.. = Len
        uint16_t header = (uint16_t)((s.size() << Truth::FNameEntryToLenBit) | 0u);
        memcpy(entry, &header, sizeof(header));
        memcpy(entry + Truth::FNameEntryToString, s.data(), s.size());

        uint32_t id = (block << 16) | (uint32_t)(cursor / Truth::FNameStride);

        GNamesPool.CurrentByteCursor = (uint32_t)(cursor + need);

        names.push_back(s);
        ids.push_back(id);
        return id;
    }

    // UE stores wide names as UTF-16 — two bytes per char. A dumper that
    // advances by sizeof(wchar_t) (4 on Android) desynchronises here.
    uint32_t addWide(const std::string& ascii) {
        uint32_t block  = GNamesPool.CurrentBlock;
        uint32_t cursor = GNamesPool.CurrentByteCursor;

        size_t need = Truth::FNameEntryToString + ascii.size() * 2;
        need = (need + Truth::FNameStride - 1) & ~(Truth::FNameStride - 1);

        if (cursor + need + Truth::FNameStride > Truth::BlockSizeBytes) {
            block += 1; cursor = 0;
            GNamesPool.CurrentBlock = block;
            GNamesPool.Blocks[block] = (uint8_t*)calloc(1, Truth::BlockSizeBytes);
        }

        uint8_t* entry = GNamesPool.Blocks[block] + cursor;
        uint16_t header = (uint16_t)((ascii.size() << Truth::FNameEntryToLenBit) | 1u);
        memcpy(entry, &header, sizeof(header));
        for (size_t i = 0; i < ascii.size(); ++i) {
            uint16_t c = (uint8_t)ascii[i];
            memcpy(entry + Truth::FNameEntryToString + i * 2, &c, 2);
        }

        uint32_t id = (block << 16) | (uint32_t)(cursor / Truth::FNameStride);
        GNamesPool.CurrentByteCursor = (uint32_t)(cursor + need);
        names.push_back(ascii);
        ids.push_back(id);
        return id;
    }
};

static NameTable g_names;

// ---------------------------------------------------------------------------
// Object array
// ---------------------------------------------------------------------------
static std::vector<uint8_t*> g_chunks;
static uint8_t**             g_chunkList = nullptr;
static uint32_t              g_numObjects = 0;

static void initObjectArray() {
    g_chunks.push_back((uint8_t*)calloc(Truth::ObjectsPerChunk, Truth::FUObjectItemSize));

    g_chunkList = (uint8_t**)calloc(64, sizeof(uint8_t*));
    uint8_t** chunkList = g_chunkList;
    chunkList[0] = g_chunks[0];

    uint8_t* tuobj = GUObjectArray + Truth::FUObjectArrayToTUObjectArray;
    W64(tuobj, Truth::TUObjectArrayToObjects,     (uint64_t)(uintptr_t)chunkList);
    W32(tuobj, Truth::TUObjectArrayToMaxElements, Truth::ObjectsPerChunk);
    W32(tuobj, Truth::TUObjectArrayToNumElements, 0);
}

// Allocate an object of `size` bytes, register it in the object array, and
// fill the UObject header. Returns the object pointer.
static void* newObject(size_t size, uint32_t nameId, void* clazz, void* outer) {
    void* obj = calloc(1, size);

    uint32_t index = g_numObjects++;

    W64(obj, Truth::UObjectToVTable,        (uint64_t)(uintptr_t)FakeVTable);
    W32(obj, Truth::UObjectToObjectFlags,   0x0);
    W32(obj, Truth::UObjectToInternalIndex, index);
    W64(obj, Truth::UObjectToClassPrivate,  (uint64_t)(uintptr_t)clazz);
    W32(obj, Truth::UObjectToFNameIndex,    nameId);
    W32(obj, Truth::UObjectToFNameIndex + 4, 0);  // FName::Number
    W64(obj, Truth::UObjectToOuterPrivate,  (uint64_t)(uintptr_t)outer);

    size_t chunkIdx = index / Truth::ObjectsPerChunk;
    while (chunkIdx >= g_chunks.size()) {
        g_chunks.push_back((uint8_t*)calloc(Truth::ObjectsPerChunk, Truth::FUObjectItemSize));
        g_chunkList[g_chunks.size() - 1] = g_chunks.back();
    }

    uint8_t* item = g_chunks[chunkIdx]
                  + Truth::FUObjectItemPadd
                  + (index % Truth::ObjectsPerChunk) * Truth::FUObjectItemSize;
    W64(item, 0x00, (uint64_t)(uintptr_t)obj);  // Object
    W32(item, 0x08, 0);                         // Flags
    W32(item, 0x0C, (uint32_t)-1);              // ClusterRootIndex
    W32(item, 0x10, index + 1);                 // SerialNumber

    uint8_t* tuobj = GUObjectArray + Truth::FUObjectArrayToTUObjectArray;
    W32(tuobj, Truth::TUObjectArrayToNumElements, g_numObjects);

    return obj;
}

static void* newClass(const char* name, void* super, void* metaClass, void* outer) {
    uint32_t id = g_names.add(name);
    void* c = newObject(Truth::SizeOfUStruct, id, metaClass, outer);
    W64(c, Truth::UStructToSuperStruct, (uint64_t)(uintptr_t)super);
    W64(c, Truth::UStructToChildren,    0);
    W32(c, Truth::UStructToPropertiesSize, 0x28);
    return c;
}

static void* newProperty(const char* name, void* propClass, void* outerClass,
                         uint32_t elementSize, uint32_t offsetInternal, uint32_t arrayDim) {
    uint32_t id = g_names.add(name);
    void* p = newObject(Truth::SizeOfUProperty, id, propClass, outerClass);
    W32(p, Truth::UPropertyToArrayDim,       arrayDim);
    W32(p, Truth::UPropertyToElementSize,    elementSize);
    W64(p, Truth::UPropertyToPropertyFlags,  0x0000000000000001ull);
    W32(p, Truth::UPropertyToOffsetInternal, offsetInternal);
    return p;
}

// An FFieldClass is not a UObject — it is a plain struct whose FName sits at
// +0x00, and it never appears in the object array.
static void* newFFieldClass(const char* name) {
    uint32_t id = g_names.add(name);
    void* c = calloc(1, Truth::SizeOfFFieldClass);
    W32(c, 0x00, id);
    return c;
}

static void* newFProperty(const char* name, void* fieldClass,
                          uint32_t elementSize, uint32_t offsetInternal, uint32_t arrayDim) {
    uint32_t id = g_names.add(name);
    void* p = calloc(1, Truth::SizeOfFProperty);
    W64(p, Truth::FFieldToClass, (uint64_t)(uintptr_t)fieldClass);
    W32(p, Truth::FFieldToName, id);
    W32(p, Truth::FPropertyToArrayDim, arrayDim);
    W32(p, Truth::FPropertyToElementSize, elementSize);
    W32(p, Truth::FPropertyToOffsetInternal, offsetInternal);
    return p;
}

static void attachChildProperties(void* clazz, const std::vector<void*>& props) {
    if (props.empty()) return;
    W64(clazz, Truth::UStructToChildProperties, (uint64_t)(uintptr_t)props[0]);
    for (size_t i = 0; i + 1 < props.size(); ++i)
        W64(props[i], Truth::FFieldToNext, (uint64_t)(uintptr_t)props[i + 1]);
}

// Chain `props` onto `clazz` via UStruct::Children / UField::Next.
static void attachChildren(void* clazz, const std::vector<void*>& props) {
    if (props.empty()) return;
    W64(clazz, Truth::UStructToChildren, (uint64_t)(uintptr_t)props[0]);
    for (size_t i = 0; i + 1 < props.size(); ++i)
        W64(props[i], Truth::UFieldToNext, (uint64_t)(uintptr_t)props[i + 1]);
}

// ---------------------------------------------------------------------------
// Scene construction
// ---------------------------------------------------------------------------
struct Stats {
    uint32_t fillerNames = 0;
    uint32_t wideNames = 0;
    uint32_t longNames = 0;
    uint32_t actors = 0;
    uint32_t extraObjects = 0;
};
static Stats g_stats;

static void buildWorld(uint32_t fillerNames, uint32_t extraObjects) {
    memset(&GNamesPool, 0, sizeof(GNamesPool));
    GNamesPool.Blocks[0] = (uint8_t*)calloc(1, Truth::BlockSizeBytes);
    GNamesPool.CurrentBlock = 0;
    GNamesPool.CurrentByteCursor = 0;

    // Index 0 must decode to "None" — every validator keys off this.
    g_names.add("None");

    initObjectArray();

    // Bootstrap: UClass is its own class, as in real UE4.
    void* classClass = newClass("Class", nullptr, nullptr, nullptr);
    W64(classClass, Truth::UObjectToClassPrivate, (uint64_t)(uintptr_t)classClass);

    void* packageClass = newClass("Package", nullptr, classClass, nullptr);
    void* bpGenClass   = newClass("BlueprintGeneratedClass", classClass, classClass, nullptr);

    // Packages, so outer chains produce real paths like "Engine.Actor".
    void* pkgCore = newObject(Truth::SizeOfUObject, g_names.add("CoreUObject"), packageClass, nullptr);
    void* pkgEng  = newObject(Truth::SizeOfUObject, g_names.add("Engine"),      packageClass, nullptr);
    void* pkgGame = newObject(Truth::SizeOfUObject, g_names.add("MyGame"),      packageClass, nullptr);

    void* objectClass   = newClass("Object",   nullptr,     classClass, pkgCore);
    void* fieldClass    = newClass("Field",    objectClass, classClass, pkgCore);
    void* structClass   = newClass("Struct",   fieldClass,  classClass, pkgCore);
    void* propertyClass = newClass("Property", fieldClass,  classClass, pkgCore);
    (void)structClass;

    newClass("IntProperty",    propertyClass, classClass, pkgCore);
    void* floatPropClass  = newClass("FloatProperty",  propertyClass, classClass, pkgCore);
    void* boolPropClass   = newClass("BoolProperty",   propertyClass, classClass, pkgCore);
    void* objectPropClass = newClass("ObjectProperty", propertyClass, classClass, pkgCore);
    void* bytePropClass   = newClass("ByteProperty",   propertyClass, classClass, pkgCore);
    newClass("ArrayProperty",  propertyClass, classClass, pkgCore);

    void* actorClass = newClass("Actor",     objectClass, classClass, pkgEng);
    void* pawnClass  = newClass("Pawn",      actorClass,  classClass, pkgEng);
    void* charClass  = newClass("Character", pawnClass,   classClass, pkgEng);
    void* worldClass = newClass("World",     objectClass, classClass, pkgEng);
    void* levelClass = newClass("Level",     objectClass, classClass, pkgEng);
    void* ctrlClass  = newClass("PlayerController", actorClass, classClass, pkgEng);

    attachChildren(actorClass, {
        newProperty("bHidden",            boolPropClass,   actorClass, 0x01, 0x38,  1),
        newProperty("CustomTimeDilation", floatPropClass,  actorClass, 0x04, 0x3C,  1),
        newProperty("RootComponent",      objectPropClass, actorClass, 0x08, 0x180, 1),
    });
    attachChildren(pawnClass, {
        newProperty("BaseEyeHeight", floatPropClass,  pawnClass, 0x04, 0x300, 1),
        newProperty("Controller",    objectPropClass, pawnClass, 0x08, 0x308, 1),
    });
    attachChildren(charClass, {
        newProperty("Health",         floatPropClass,  charClass, 0x04, 0x400, 1),
        newProperty("MaxHealth",      floatPropClass,  charClass, 0x04, 0x404, 1),
        newProperty("TeamId",         bytePropClass,   charClass, 0x01, 0x408, 1),
        newProperty("Mesh",           objectPropClass, charClass, 0x08, 0x410, 1),
        // ArrayDim > 1 on purpose: upstream reads ArrayDim at ElementSize's
        // offset, so this comes back as 8 instead of 4.
        newProperty("InventorySlots", objectPropClass, charClass, 0x08, 0x418, 4),
    });
    attachChildren(ctrlClass, {
        newProperty("PlayerCameraManager", objectPropClass, ctrlClass, 0x08, 0x200, 1),
    });

    // Blueprint-generated classes, as a packaged game would have.
    void* bpHero  = newClass("BP_Hero_C",  charClass, bpGenClass, pkgGame);
    void* bpEnemy = newClass("BP_Enemy_C", charClass, bpGenClass, pkgGame);
    // Deliberately built the 4.25+ way: these two carry FProperty on
    // ChildProperties, while Actor/Pawn/Character above still use the legacy
    // UProperty chain. One run therefore exercises both layouts.
    void* fcFloat  = newFFieldClass("FloatProperty");
    void* fcObject = newFFieldClass("ObjectProperty");
    void* fcBool   = newFFieldClass("BoolProperty");

    attachChildProperties(bpHero, {
        newFProperty("Stamina",     fcFloat,  0x04, 0x500, 1),
        newFProperty("Loadout",     fcObject, 0x08, 0x508, 3),
        newFProperty("bIsSprinting", fcBool,  0x01, 0x520, 1),
    });
    attachChildProperties(bpEnemy, {
        newFProperty("AggroRange",  fcFloat,  0x04, 0x500, 1),
    });

    // The world chain: GWorld -> UWorld -> PersistentLevel -> Actors[]
    void* world = newObject(0x800, g_names.add("TestLevel_Main"), worldClass, pkgGame);
    void* level = newObject(0x200, g_names.add("PersistentLevel"), levelClass, world);
    W64(world, Truth::UWorldToPersistentLevel, (uint64_t)(uintptr_t)level);

    const int kActors = 64;
    void** actors = (void**)calloc(kActors, sizeof(void*));
    for (int i = 0; i < kActors; ++i) {
        char nm[64];
        snprintf(nm, sizeof(nm), "BP_Hero_C_%d", i);
        void* a = newObject(0x600, g_names.add(nm), (i % 3) ? bpEnemy : bpHero, level);
        float pos[3] = { (float)(i * 137.5f), (float)(i * -82.25f), 92.0f };
        memcpy((char*)a + 0x1C0, pos, sizeof(pos));
        W32(a, 0x400, 0x42C80000);  // Health = 100.0f
        actors[i] = a;
    }
    g_stats.actors = kActors;

    W64(level, Truth::ULevelToAActors,      (uint64_t)(uintptr_t)actors);
    W32(level, Truth::ULevelToAActorsCount, kActors);
    GWorld = world;

    // Wide names. ASCII content, but stored UTF-16 with the wide bit set.
    g_names.addWide("WideName_Alpha");
    g_names.addWide("WideName_Beta_LongerOne");
    g_stats.wideNames = 2;

    // A name past 511 chars: with a SIGNED header read the length shifts
    // negative and the name silently becomes "None".
    g_names.add(std::string(600, 'L'));
    g_stats.longNames = 1;

    // Filler to push the pool past one 128 KiB block, so block indexing and
    // the per-block offset arithmetic are both exercised.
    for (uint32_t i = 0; i < fillerNames; ++i) {
        char nm[64];
        snprintf(nm, sizeof(nm), "FillerName_%06u_Padding", i);
        g_names.add(nm);
    }
    g_stats.fillerNames = fillerNames;

    // Names created AFTER the filler land in block 1+, so Objects.txt has
    // entries whose ids only resolve if block indexing is right.
    void* lateClass = newClass("LateBlockClass", objectClass, classClass, pkgGame);
    attachChildren(lateClass, {
        newProperty("LateValue", floatPropClass, lateClass, 0x04, 0x40, 1),
    });

    // Optional bulk objects, to cross a 65536-object chunk boundary.
    for (uint32_t i = 0; i < extraObjects; ++i) {
        char nm[64];
        snprintf(nm, sizeof(nm), "FillerObject_%06u", i);
        newObject(Truth::SizeOfUObject, g_names.add(nm), objectClass, pkgGame);
    }
    g_stats.extraObjects = extraObjects;
}

// ---------------------------------------------------------------------------
// Module base, so we can report globals the way a dumper sees them: as
// offsets from the start of the loaded image.
// ---------------------------------------------------------------------------
static uintptr_t g_moduleBase = 0;

// dladdr on a symbol we own gives the load base of the containing module.
// NB: dl_iterate_phdr is NOT usable here — on bionic the first entry is not
// the main executable, which silently yields a bogus base.
static uintptr_t moduleBase() {
    Dl_info info;
    if (dladdr((void*)&moduleBase, &info) && info.dli_fbase)
        return (uintptr_t)info.dli_fbase;
    return 0;
}

// ---------------------------------------------------------------------------
// Reference walk — what a correct dumper should produce.
// ---------------------------------------------------------------------------
static std::string nameFromId(uint32_t id) {
    uint32_t block  = id >> 16;
    uint16_t offset = (uint16_t)(id & 0xFFFF);

    uint8_t* chunk = GNamesPool.Blocks[block];
    if (!chunk) return "<bad block>";

    uint8_t* entry = chunk + (size_t)offset * Truth::FNameStride;
    uint16_t header = R16(entry, 0);                       // unsigned, deliberately
    uint32_t len    = header >> Truth::FNameEntryToLenBit;
    bool     wide   = (header & 1) != 0;

    if (len == 0 || len > 1024) return "<bad len>";
    if (wide) return "<wide unsupported>";

    return std::string((char*)entry + Truth::FNameEntryToString, len);
}

static void* objectFromId(uint32_t index) {
    uint8_t* tuobj = GUObjectArray + Truth::FUObjectArrayToTUObjectArray;
    uint8_t** chunks = (uint8_t**)(uintptr_t)R64(tuobj, Truth::TUObjectArrayToObjects);
    uint8_t* chunk = chunks[index / Truth::ObjectsPerChunk];
    uint8_t* item = chunk + Truth::FUObjectItemPadd
                  + (index % Truth::ObjectsPerChunk) * Truth::FUObjectItemSize;
    return (void*)(uintptr_t)R64(item, 0x00);
}

static void selftest() {
    uint8_t* tuobj = GUObjectArray + Truth::FUObjectArrayToTUObjectArray;
    uint32_t count = R32(tuobj, Truth::TUObjectArrayToNumElements);

    printf("\n=== selftest: reference walk ===\n");
    printf("NumElements            : %u\n", count);
    printf("name[0]                : \"%s\"  (must be \"None\")\n", nameFromId(0).c_str());

    printf("\nfirst 12 objects:\n");
    for (uint32_t i = 0; i < count && i < 12; ++i) {
        void* o = objectFromId(i);
        uint32_t nameId  = R32(o, Truth::UObjectToFNameIndex);
        void*    clazz   = (void*)(uintptr_t)R64(o, Truth::UObjectToClassPrivate);
        uint32_t classNm = clazz ? R32(clazz, Truth::UObjectToFNameIndex) : 0;
        printf("  [%3u] %-22s class=%-16s ptr=%p\n",
               i, nameFromId(nameId).c_str(),
               clazz ? nameFromId(classNm).c_str() : "<null>", o);
    }

    printf("\nGWorld chain:\n");
    void* world = GWorld;
    void* level = (void*)(uintptr_t)R64(world, Truth::UWorldToPersistentLevel);
    uint32_t actorCount = R32(level, Truth::ULevelToAActorsCount);
    void** actors = (void**)(uintptr_t)R64(level, Truth::ULevelToAActors);
    printf("  UWorld   %p  \"%s\"\n", world, nameFromId(R32(world, Truth::UObjectToFNameIndex)).c_str());
    printf("  ULevel   %p  \"%s\"\n", level, nameFromId(R32(level, Truth::UObjectToFNameIndex)).c_str());
    printf("  Actors   %u\n", actorCount);
    for (uint32_t i = 0; i < actorCount && i < 4; ++i) {
        float pos[3];
        memcpy(pos, (char*)actors[i] + 0x1C0, sizeof(pos));
        printf("    [%2u] %-20s pos=(%.1f, %.1f, %.1f)\n", i,
               nameFromId(R32(actors[i], Truth::UObjectToFNameIndex)).c_str(),
               pos[0], pos[1], pos[2]);
    }

    printf("\nproperties of Character:\n");
    for (uint32_t i = 0; i < count; ++i) {
        void* o = objectFromId(i);
        if (nameFromId(R32(o, Truth::UObjectToFNameIndex)) != "Character") continue;
        void* child = (void*)(uintptr_t)R64(o, Truth::UStructToChildren);
        while (child) {
            printf("    %-18s offset=0x%-4X size=0x%-3X dim=%u\n",
                   nameFromId(R32(child, Truth::UObjectToFNameIndex)).c_str(),
                   R32(child, Truth::UPropertyToOffsetInternal),
                   R32(child, Truth::UPropertyToElementSize),
                   R32(child, Truth::UPropertyToArrayDim));
            child = (void*)(uintptr_t)R64(child, Truth::UFieldToNext);
        }
        break;
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
static void printTruth() {
    uintptr_t base = g_moduleBase;

    printf("=== ue4_fake ground truth ===\n");
    printf("pid                    : %d\n", getpid());
    printf("module base            : 0x%zx\n", (size_t)base);
    printf("\nglobals (absolute / module-relative):\n");
    printf("  GNames               : 0x%zx  +0x%zx   (inline FNamePool, NOT a pointer)\n",
           (size_t)(uintptr_t)&GNamesPool, (size_t)((uintptr_t)&GNamesPool - base));
    printf("  GUObjectArray        : 0x%zx  +0x%zx   (inline FUObjectArray, NOT a pointer)\n",
           (size_t)(uintptr_t)GUObjectArray, (size_t)((uintptr_t)GUObjectArray - base));
    printf("  GWorld               : 0x%zx  +0x%zx   (pointer to UWorld)\n",
           (size_t)(uintptr_t)&GWorld, (size_t)((uintptr_t)&GWorld - base));

    printf("\nstruct offsets a dumper must use against this target:\n");
    printf("  GNamesToFNamePool            0x%02zx\n", (size_t)0);
    printf("  FNamePoolToCurrentBlock      0x%02zx\n", Truth::FNamePoolToCurrentBlock);
    printf("  FNamePoolToCurrentByteCursor 0x%02zx\n", Truth::FNamePoolToCurrentByteCursor);
    printf("  FNamePoolToBlocks            0x%02zx\n", Truth::FNamePoolToBlocks);
    printf("  FNameStride                  0x%02zx\n", Truth::FNameStride);
    printf("  FNameEntryToString           0x%02zx\n", Truth::FNameEntryToString);
    printf("  FNameEntryToLenBit           %zu\n",     Truth::FNameEntryToLenBit);
    printf("  FUObjectArrayToTUObjectArray 0x%02zx\n", Truth::FUObjectArrayToTUObjectArray);
    printf("  TUObjectArrayToNumElements   0x%02zx\n", Truth::TUObjectArrayToNumElements);
    printf("  FUObjectItemSize             0x%02zx\n", Truth::FUObjectItemSize);
    printf("  UObjectToInternalIndex       0x%02zx\n", Truth::UObjectToInternalIndex);
    printf("  UObjectToClassPrivate        0x%02zx\n", Truth::UObjectToClassPrivate);
    printf("  UObjectToFNameIndex          0x%02zx\n", Truth::UObjectToFNameIndex);
    printf("  UObjectToOuterPrivate        0x%02zx\n", Truth::UObjectToOuterPrivate);
    printf("  UFieldToNext                 0x%02zx\n", Truth::UFieldToNext);
    printf("  UStructToSuperStruct         0x%02zx\n", Truth::UStructToSuperStruct);
    printf("  UStructToChildren            0x%02zx\n", Truth::UStructToChildren);
    printf("  UPropertyToArrayDim          0x%02zx\n", Truth::UPropertyToArrayDim);
    printf("  UPropertyToElementSize       0x%02zx\n", Truth::UPropertyToElementSize);
    printf("  UPropertyToOffsetInternal    0x%02zx\n", Truth::UPropertyToOffsetInternal);
    printf("  UWorldToPersistentLevel      0x%02zx\n", Truth::UWorldToPersistentLevel);
    printf("  ULevelToAActors              0x%02zx\n", Truth::ULevelToAActors);
    printf("  ULevelToAActorsCount         0x%02zx\n", Truth::ULevelToAActorsCount);

    uint8_t* tuobj = GUObjectArray + Truth::FUObjectArrayToTUObjectArray;
    printf("\ncounts:\n");
    printf("  names                : %zu\n", g_names.names.size());
    printf("  objects              : %u\n", R32(tuobj, Truth::TUObjectArrayToNumElements));
    printf("  name blocks used     : %u (cursor 0x%x)\n",
           GNamesPool.CurrentBlock + 1, GNamesPool.CurrentByteCursor);

    printf("\nsample name ids:\n");
    for (size_t i = 0; i < g_names.names.size() && i < 8; ++i)
        printf("  [0x%08x] %s\n", g_names.ids[i], g_names.names[i].c_str());
}

static void writeJson(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { perror("fopen"); return; }
    uintptr_t base = g_moduleBase;
    uint8_t* tuobj = GUObjectArray + Truth::FUObjectArrayToTUObjectArray;

    fprintf(f, "{\n");
    fprintf(f, "  \"pid\": %d,\n", getpid());
    fprintf(f, "  \"module_base\": \"0x%zx\",\n", (size_t)base);
    fprintf(f, "  \"globals\": {\n");
    fprintf(f, "    \"GNames\":        {\"abs\": \"0x%zx\", \"rel\": \"0x%zx\", \"deref\": false},\n",
            (size_t)(uintptr_t)&GNamesPool, (size_t)((uintptr_t)&GNamesPool - base));
    fprintf(f, "    \"GUObjectArray\": {\"abs\": \"0x%zx\", \"rel\": \"0x%zx\", \"deref\": false},\n",
            (size_t)(uintptr_t)GUObjectArray, (size_t)((uintptr_t)GUObjectArray - base));
    fprintf(f, "    \"GWorld\":        {\"abs\": \"0x%zx\", \"rel\": \"0x%zx\", \"deref\": true}\n",
            (size_t)(uintptr_t)&GWorld, (size_t)((uintptr_t)&GWorld - base));
    fprintf(f, "  },\n");
    fprintf(f, "  \"offsets\": {\n");
    fprintf(f, "    \"FNamePoolToCurrentBlock\": %zu,\n", Truth::FNamePoolToCurrentBlock);
    fprintf(f, "    \"FNamePoolToCurrentByteCursor\": %zu,\n", Truth::FNamePoolToCurrentByteCursor);
    fprintf(f, "    \"FNamePoolToBlocks\": %zu,\n", Truth::FNamePoolToBlocks);
    fprintf(f, "    \"FNameStride\": %zu,\n", Truth::FNameStride);
    fprintf(f, "    \"FNameEntryToString\": %zu,\n", Truth::FNameEntryToString);
    fprintf(f, "    \"FNameEntryToLenBit\": %zu,\n", Truth::FNameEntryToLenBit);
    fprintf(f, "    \"FUObjectArrayToTUObjectArray\": %zu,\n", Truth::FUObjectArrayToTUObjectArray);
    fprintf(f, "    \"TUObjectArrayToNumElements\": %zu,\n", Truth::TUObjectArrayToNumElements);
    fprintf(f, "    \"FUObjectItemSize\": %zu,\n", Truth::FUObjectItemSize);
    fprintf(f, "    \"UObjectToInternalIndex\": %zu,\n", Truth::UObjectToInternalIndex);
    fprintf(f, "    \"UObjectToClassPrivate\": %zu,\n", Truth::UObjectToClassPrivate);
    fprintf(f, "    \"UObjectToFNameIndex\": %zu,\n", Truth::UObjectToFNameIndex);
    fprintf(f, "    \"UObjectToOuterPrivate\": %zu,\n", Truth::UObjectToOuterPrivate);
    fprintf(f, "    \"UFieldToNext\": %zu,\n", Truth::UFieldToNext);
    fprintf(f, "    \"UStructToSuperStruct\": %zu,\n", Truth::UStructToSuperStruct);
    fprintf(f, "    \"UStructToChildren\": %zu,\n", Truth::UStructToChildren);
    fprintf(f, "    \"UPropertyToArrayDim\": %zu,\n", Truth::UPropertyToArrayDim);
    fprintf(f, "    \"UPropertyToElementSize\": %zu,\n", Truth::UPropertyToElementSize);
    fprintf(f, "    \"UPropertyToOffsetInternal\": %zu,\n", Truth::UPropertyToOffsetInternal);
    fprintf(f, "    \"UWorldToPersistentLevel\": %zu,\n", Truth::UWorldToPersistentLevel);
    fprintf(f, "    \"ULevelToAActors\": %zu,\n", Truth::ULevelToAActors);
    fprintf(f, "    \"ULevelToAActorsCount\": %zu\n", Truth::ULevelToAActorsCount);
    fprintf(f, "  },\n");
    fprintf(f, "  \"expect\": {\n");
    fprintf(f, "    \"name_count\": %zu,\n", g_names.names.size());
    fprintf(f, "    \"object_count\": %u,\n", R32(tuobj, Truth::TUObjectArrayToNumElements));
    fprintf(f, "    \"actor_count\": %u,\n", g_stats.actors);
    fprintf(f, "    \"filler_names\": %u,\n", g_stats.fillerNames);
    fprintf(f, "    \"wide_names\": %u,\n", g_stats.wideNames);
    fprintf(f, "    \"long_names\": %u,\n", g_stats.longNames);
    fprintf(f, "    \"name_blocks\": %u,\n", GNamesPool.CurrentBlock + 1);
    fprintf(f, "    \"name_0\": \"None\"\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"names\": [\n");
    for (size_t i = 0; i < g_names.names.size(); ++i)
        fprintf(f, "    {\"id\": %u, \"name\": \"%s\"}%s\n",
                g_names.ids[i], g_names.names[i].c_str(),
                i + 1 == g_names.names.size() ? "" : ",");
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("\nwrote ground truth JSON -> %s\n", path);
}

int main(int argc, char** argv) {
    int holdSeconds = 0;
    bool doSelftest = false;
    const char* jsonPath = nullptr;
    const char* selfDumpDir = nullptr;
    uint32_t fillerNames = 8000;     // enough to cross one 128 KiB block
    uint32_t extraObjects = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--selftest")) doSelftest = true;
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) holdSeconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) jsonPath = argv[++i];
        else if (!strcmp(argv[i], "--filler") && i + 1 < argc) fillerNames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--big")) extraObjects = 70000;
        else if (!strcmp(argv[i], "--selfdump") && i + 1 < argc) selfDumpDir = argv[++i];
    }

    g_moduleBase = moduleBase();
    buildWorld(fillerNames, extraObjects);
    printTruth();

    if (doSelftest) selftest();
    if (jsonPath)   writeJson(jsonPath);

    if (selfDumpDir) {
        // pid 0 = read our own memory. Discovery is skipped: we hand the
        // emitter the layout we built, so this tests emission alone.
        ue4::Reader R{ 0 };
        ue4::PoolLayout P;
        P.base = (uintptr_t)&GNamesPool;
        ue4::ObjLayout O;
        O.fuObjectArray = (uintptr_t)GUObjectArray;
        O.toNumElements = Truth::TUObjectArrayToNumElements;
        O.numElements   = R.get<uint32_t>(O.fuObjectArray + O.toTUObjectArray + O.toNumElements);

        ue4::EmitResult r = ue4::emitDump(R, P, O, g_moduleBase, "ue4_fake(self)",
                                          selfDumpDir, true,
                                          (uintptr_t)&GWorld, (uintptr_t)GWorld);
        printf("\nselfdump -> %s   names=%u objects=%u classes=%u\n",
               selfDumpDir, r.names, r.objects, r.classes);
    }

    if (holdSeconds > 0) {
        printf("\nholding %d seconds for attach (pid %d)...\n", holdSeconds, getpid());
        fflush(stdout);
        sleep(holdSeconds);
    }
    return 0;
}
