#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace imsrv::deviceproof {
using Bytes = std::vector<std::uint8_t>;

Bytes message(const std::string& operation, const Bytes& appSessionId,
              const std::string& deviceId, const std::string& credentialBinding,
              const Bytes& publicKey = {});
bool verifyP256(const Bytes& publicKeyDer, const Bytes& message, const Bytes& signatureDer);
} // namespace imsrv::deviceproof
