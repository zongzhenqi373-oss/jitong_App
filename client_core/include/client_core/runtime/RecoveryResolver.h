#pragma once
#include <cstdint>

namespace im { namespace runtime {
// Evidence must be authenticated and bound to the selected owner before marking it valid.
enum class CutoverState { Missing, Corrupt, Legacy, Prepared, NoWrite, Dirty };
enum class RecoveryAction { Legacy, Native, Repair };
struct CutoverEvidence {
    CutoverState state = CutoverState::Missing;
    std::int64_t ownerId = 0;
    std::int64_t epoch = 0;
};

inline RecoveryAction resolveRecovery(std::int64_t ownerId, CutoverEvidence db,
    CutoverEvidence mirror, bool nativeExists, bool nativeLoadable)
{
    using S=CutoverState;
    using A=RecoveryAction;
    const auto absent=[](CutoverEvidence e){return e.state==S::Missing;};
    const auto valid=[&](CutoverEvidence e){return e.state!=S::Corrupt &&
        (absent(e)||(e.ownerId==ownerId && e.epoch>0));};
    if(ownerId<=0 || !valid(db) || !valid(mirror)) return A::Repair;
    if(absent(db) && absent(mirror)) return nativeExists?A::Repair:A::Legacy;
    // No automatic fallback on incomplete, divergent, or stale evidence, even PREPARED.
    if(absent(db) || absent(mirror) || db.state!=mirror.state || db.epoch!=mirror.epoch)
        return A::Repair;
    if(db.state==S::Legacy || db.state==S::Prepared) return A::Legacy;
    if(db.state==S::NoWrite || db.state==S::Dirty)
        return nativeExists && nativeLoadable ? A::Native : A::Repair;
    return A::Repair;
}
} }
