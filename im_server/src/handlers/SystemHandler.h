#pragma once

#include <memory>
#include <string>

namespace imsrv {
class Presence;
class Session;

class SystemHandler {
public:
    explicit SystemHandler(Presence& presence);
    void onOffline(const std::shared_ptr<Session>& session, const std::string& payload);
    void onHeartbeat(const std::shared_ptr<Session>& session, const std::string& payload);

private:
    Presence& m_presence;
};
} // namespace imsrv
