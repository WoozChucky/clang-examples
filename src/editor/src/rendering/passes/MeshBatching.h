#pragma once
#include <cstdint>
#include <algorithm>

// One visible mesh entity tagged with its batch key. POD; arena-friendly.
struct BatchEntry {
    uint32_t meshId;
    uint32_t materialId;
    uint64_t entity;   // EntityId
};

// A contiguous run of entries sharing one (meshId, materialId) == one draw batch.
struct BatchRun {
    uint32_t begin;    // index into entries
    uint32_t count;
};

// Sorts entries[0..count) by (meshId, materialId) in place, then fills `runs`
// with the contiguous equal-key runs. Returns the number of runs (<= count).
// Caller must size `runs` to at least `count` (worst case: all-distinct keys).
// Stops early if the run count would exceed `maxRuns`.
inline uint32_t BuildBatchRuns(BatchEntry* entries, uint32_t count,
                               BatchRun* runs, uint32_t maxRuns) {
    if (count == 0 || !entries || !runs || maxRuns == 0) return 0;
    std::sort(entries, entries + count, [](const BatchEntry& a, const BatchEntry& b) {
        if (a.meshId != b.meshId) return a.meshId < b.meshId;
        return a.materialId < b.materialId;
    });
    uint32_t runCount = 0;
    uint32_t i = 0;
    while (i < count && runCount < maxRuns) {
        uint32_t j = i + 1;
        while (j < count &&
               entries[j].meshId == entries[i].meshId &&
               entries[j].materialId == entries[i].materialId) {
            ++j;
        }
        runs[runCount++] = BatchRun{ i, j - i };
        i = j;
    }
    return runCount;
}
