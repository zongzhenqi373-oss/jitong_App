#include "client_core/storage/DbKeyBridge.h"

namespace im {
namespace storage {

const char* toString(IPlatformKeyBridge::Result r)
{
    switch (r) {
        case IPlatformKeyBridge::Result::Ok:          return "Ok";
        case IPlatformKeyBridge::Result::Unavailable: return "Unavailable";
        case IPlatformKeyBridge::Result::Error:       return "Error";
    }
    return "?";
}

} // namespace storage
} // namespace im
