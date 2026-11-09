// ue4scan — locate GNames, GUObjectArray and GWorld in a running UE4 process
// with no prior offsets, and report them module-relative.
//
// Usage: ue4scan <pid> [--module <substr>] [--all] [--verbose]

#include "../common/ue4core.h"

using namespace ue4;

static void compareWithUpstream(const ObjLayout& O) {
    struct Row { const char* name; size_t upstream; size_t actual; };
    const Row rows[] = {
        { "FUObjectArrayToTUObjectArray", 0x10, O.toTUObjectArray   },
        { "TUObjectArrayToNumElements",   0x0C, O.toNumElements     },
        { "FUObjectItemSize",             0x18, O.itemSize          },
        { "UObjectToClassPrivate",        0x10, O.objToClass        },
        { "UObjectToFNameIndex",          0x18, O.objToName         },
        { "UObjectToOuterPrivate",        0x20, O.objToOuter        },
        { "UFieldToNext",                 0x28, O.fieldToNext       },
        { "UStructToSuperStruct",         0x30, O.structToSuper     },
        { "UStructToChildren",            0x38, O.structToChildren  },
        { "UPropertyToElementSize",       0x34, O.propToElementSize },
        { "UPropertyToOffsetInternal",    0x44, O.propToOffset      },
        { "UProperty ArrayDim",           0x34, O.propToArrayDim    },
    };
    printf("\nvs UE4Dumper initOffsets_64 defaults:\n");
    for (const auto& r : rows)
        printf("  %-30s upstream 0x%02zx   ours 0x%02zx   %s\n",
               r.name, r.upstream, r.actual,
               r.upstream == r.actual ? "ok" : "<-- MISMATCH");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pid> [--module <substr>] [--all] [--verbose]\n", argv[0]);
        return 2;
    }

    Reader R{ (pid_t)atoi(argv[1]) };
    std::string moduleFilter;
    bool all = false, verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) moduleFilter = argv[++i];
        else if (!strcmp(argv[i], "--all")) all = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
    }

    auto regions = readMaps(R.pid);
    if (regions.empty()) { fprintf(stderr, "no maps for pid %d\n", R.pid); return 1; }

    std::string mainPath;
    uintptr_t base = moduleBase(regions, moduleFilter, &mainPath);
    // Default to the module's own writable memory — that is where the engine
    // globals live. --all falls back to every writable region in the process.
    auto scan = (!moduleFilter.empty() && !all)
              ? moduleWritableRegions(regions, moduleFilter)
              : scanCandidates(regions, all);

    printf("pid %d   module \"%s\" base 0x%zx   scanning %zu writable regions\n",
           R.pid, mainPath.c_str(), (size_t)base, scan.size());

    PoolLayout P;
    if (!findNamePool(R, regions, scan, P, verbose)) { printf("GNames: NOT FOUND\n"); return 1; }

    ObjLayout O;
    bool haveObjs = findObjectArray(R, regions, scan, P, O, verbose);

    uintptr_t gworld = 0, world = 0;
    bool haveWorld = haveObjs && findGWorld(R, regions, scan, P, O, gworld, world, verbose);

    printf("\n=== results ===\n");
    printf("GNames        0x%zx", (size_t)P.base);
    if (base) printf("  +0x%zx", (size_t)(P.base - base));
    printf("   name[0]=\"%s\"\n", decodeName(R, P, 0).c_str());

    if (haveObjs) {
        printf("GUObjectArray 0x%zx", (size_t)O.fuObjectArray);
        if (base) printf("  +0x%zx", (size_t)(O.fuObjectArray - base));
        printf("   objects=%u  obj[0]=\"%s\"\n", O.numElements,
               objectName(R, P, O, objectAt(R, O, 0)).c_str());
    } else printf("GUObjectArray NOT FOUND\n");

    if (haveWorld) {
        printf("GWorld        0x%zx", (size_t)gworld);
        if (base) printf("  +0x%zx", (size_t)(gworld - base));
        printf("   -> UWorld 0x%zx \"%s\"\n", (size_t)world, objectName(R, P, O, world).c_str());
    } else printf("GWorld        NOT FOUND\n");

    if (haveObjs) compareWithUpstream(O);
    return 0;
}
