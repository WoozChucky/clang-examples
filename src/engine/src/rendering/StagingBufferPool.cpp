#include "StagingBufferPool.h"

// Single cross-DLL instance (see header). Non-leaked Meyers singleton: the app joins both
// threads + clears LatestWorldSnapshot before static destruction, so no Acquire/Return
// races the destructor. Living in Engine.dll's TU (not inline in the header) means the
// upload code and the editor's Memory panel share the same pool.
StagingBufferPool& GetStagingPool() {
    static StagingBufferPool pool;
    return pool;
}

StagingPoolStats GetStagingPoolStats() {
    return GetStagingPool().Stats();
}
