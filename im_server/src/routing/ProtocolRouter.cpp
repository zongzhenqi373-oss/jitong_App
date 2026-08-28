#include "routing/ProtocolRouter.h"

#include "Log.h"
#include "Session.h"

#include <stdexcept>
#include <utility>

namespace imsrv {

void ProtocolRouter::add(std::uint32_t type, Handler handler)
{
    if (!handler) throw std::invalid_argument("protocol handler is empty");
    if (!m_routes.emplace(type, std::move(handler)).second) {
        throw std::logic_error("duplicate protocol route: " + std::to_string(type));
    }
}

bool ProtocolRouter::contains(std::uint32_t type) const
{
    return m_routes.find(type) != m_routes.end();
}

void ProtocolRouter::dispatch(const std::shared_ptr<Session>& session,
                              std::uint32_t type, const std::string& payload) const
{
    const auto route = m_routes.find(type);
    if (route == m_routes.end()) {
        log("[router] 未注册协议号: ", type);
        return;
    }
    try {
        route->second(session, payload);
    } catch (const std::exception& error) {
        log("[router] 协议处理异常 type=", type, " error=", error.what());
        if (session) session->close();
    } catch (...) {
        log("[router] 协议处理未知异常 type=", type);
        if (session) session->close();
    }
}

} // namespace imsrv
