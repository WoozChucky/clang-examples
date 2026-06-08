#include "animation/AnimServicesImpl.h"
#include "AnimServices.h"
#include "animation/AnimatorControllerStore.h"
#include "animation/AnimationStore.h"
#include "AnimatorController.h"   // NormalizedStateTime
#include <algorithm>
#include <string>

namespace {
float ForwardQueryStateCursor(uint64_t controllerId, int stateIndex, float stateTime, float phase,
                              char* outName, int outCap) {
    if (outName && outCap > 0) outName[0] = '\0';
    auto ctrl = AnimatorControllerStore::Instance().Get(controllerId);
    if (!ctrl) return -1.0f;
    if (stateIndex < 0 || stateIndex >= (int)ctrl->states.size()) return -1.0f;
    const AnimState& s = ctrl->states[stateIndex];
    float dur = 0.0f;
    if (stateIndex < (int)ctrl->stateClipIds.size())
        if (const auto* clip = AnimationStore::Instance().Get(ctrl->stateClipIds[stateIndex]))
            dur = clip->duration;
    if (outName && outCap > 0) {
        const int k = (int)std::min(static_cast<size_t>(outCap - 1), s.name.size());
        for (int i = 0; i < k; ++i) outName[i] = s.name[i];
        outName[k] = '\0';
    }
    return NormalizedStateTime(s, stateTime, phase, dur);
}
} // namespace

namespace AnimServicesImpl {
void Init(AnimServices& out) { out.QueryStateCursor = &ForwardQueryStateCursor; }
}
