#pragma once

#include <jni.h>
#include "client_core/runtime/ClientRuntime.h"

namespace jt {
class JniRuntimeObserver final : public im::runtime::IRuntimeEventSink {
public:
    JniRuntimeObserver(JavaVM* vm,jobject sink);
    ~JniRuntimeObserver() override;
    bool valid() const { return valid_; }
    void onInvalidated(im::runtime::InvalidationBus::Domain domain,std::int64_t dbVersion,
                       std::int64_t generation,std::int64_t ownerId) override;
    void onOperationCompleted(const im::runtime::CompletionRegistry::Record& record,
                              std::int64_t generation,std::int64_t ownerId) override;
private:
    JavaVM* vm_=nullptr;jobject sink_=nullptr;bool valid_=false;
    jmethodID invalidated_=nullptr; jmethodID completed_=nullptr;
};
}
