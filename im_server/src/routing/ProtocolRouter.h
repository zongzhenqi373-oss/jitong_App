#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace imsrv {
class Session;

class ProtocolRouter {
public:
    using Handler = std::function<void(const std::shared_ptr<Session>&, const std::string&)>;

    void add(std::uint32_t type, Handler handler);
    void dispatch(const std::shared_ptr<Session>& session,
                  std::uint32_t type, const std::string& payload) const;
    bool contains(std::uint32_t type) const;
    std::size_t size() const { return m_routes.size(); }

private:
    std::unordered_map<std::uint32_t, Handler> m_routes;
};
} // namespace imsrv
