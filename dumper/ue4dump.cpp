// ue4dump — find the globals in a running UE4 process, then emit Logs.txt,
// Names.txt, Objects.txt and a C++ SDK.
//
// Differences from kp7742/UE4Dumper that matter:
//   * nothing is supplied by hand — the globals and NumElements are found
//   * FName ids advance by the whole aligned entry, so the ids in Names.txt
//     are the real ids (upstream's drift after the first entry in a block)
//   * wide names advance 2 bytes per char, not sizeof(wchar_t)
//   * the header is read unsigned, so names >= 512 chars survive
//   * ArrayDim is read at 0x30, not at ElementSize's 0x34
//
// Usage: ue4dump <pid> [--module <substr>] [--out <dir>] [--all] [--verbose]

#include "../common/ue4emit.h"

using namespace ue4;

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pid> [--module <substr>] [--out <dir>] [--all] [--verbose]\n", argv[0]);
        return 2;
    }

    Reader R{ (pid_t)atoi(argv[1]) };
    std::string moduleFilter, outDir = "./dump";
    bool all = false, verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) moduleFilter = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
        else if (!strcmp(argv[i], "--all")) all = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
    }

    auto regions = readMaps(R.pid);
    if (regions.empty()) { fprintf(stderr, "no maps for pid %d\n", R.pid); return 1; }

    std::string mainPath;
    uintptr_t base = moduleBase(regions, moduleFilter, &mainPath);
    auto scan = scanCandidates(regions, all);

    clock_t t0 = clock();

    PoolLayout P;
    if (!findNamePool(R, regions, scan, P, verbose)) {
        fprintf(stderr, "GNames not found — cannot dump\n");
        return 1;
    }
    ObjLayout O;
    if (!findObjectArray(R, regions, scan, P, O, verbose)) {
        fprintf(stderr, "GUObjectArray not found — cannot dump\n");
        return 1;
    }
    uintptr_t gworld = 0, world = 0;
    bool haveWorld = findGWorld(R, regions, scan, P, O, gworld, world, verbose);

    EmitResult r = emitDump(R, P, O, base, mainPath, outDir, haveWorld, gworld, world);

    printf("Names    : %u\n", r.names);
    printf("Objects  : %u  (classes %u)\n", r.objects, r.classes);
    printf("done in %.2fs -> %s\n", double(clock() - t0) / CLOCKS_PER_SEC, outDir.c_str());
    return 0;
}
