#include "client_core/runtime/RecoveryResolver.h"
#include <iostream>
using namespace im::runtime;
int main() {
    using S=CutoverState; using A=RecoveryAction;
    int failures=0;
    auto check=[&](bool ok,const char* label){if(!ok){std::cerr<<label<<'\n';++failures;}};
    const S states[]={S::Missing,S::Corrupt,S::Legacy,S::Prepared,S::NoWrite,S::Dirty};
    for(auto left:states)for(auto right:states)for(bool exists:{false,true})for(bool available:{false,true}){
        CutoverEvidence db{left,1,7},kv{right,1,7};
        auto got=resolveRecovery(1,db,kv,exists,available);
        if(left==S::Dirty || right==S::Dirty || left==S::NoWrite || right==S::NoWrite)
            check(got!=A::Legacy,"committed evidence must never choose Legacy");
        if(left!=right || left==S::Corrupt)
            check(got==A::Repair,"divergent/corrupt evidence must repair");
    }
    check(resolveRecovery(1,{},{},false,false)==A::Legacy,"first install");
    check(resolveRecovery(1,{},{},true,true)==A::Repair,"orphan native database");
    check(resolveRecovery(1,{S::Dirty,1,7},{S::Dirty,1,7},true,true)==A::Native,"consistent DIRTY");
    check(resolveRecovery(1,{S::Prepared,1,7},{S::Prepared,1,7},true,true)==A::Legacy,"consistent PREPARED");
    check(resolveRecovery(1,{S::Dirty,1,7},{S::Dirty,1,8},true,true)==A::Repair,"epoch mismatch");
    check(resolveRecovery(1,{S::Legacy,2,7},{S::Legacy,2,7},true,true)==A::Repair,"owner mismatch");
    std::cout<<"144 combinations + 6 explicit checks; failures="<<failures<<'\n';
    return failures?1:0;
}
