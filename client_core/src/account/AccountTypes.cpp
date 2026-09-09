#include "client_core/AccountTypes.h"

namespace im {
namespace account {

const char* toString(AccountState s)
{
    switch (s) {
        case AccountState::LoggedOut:       return "LoggedOut";
        case AccountState::Authenticating:  return "Authenticating";
        case AccountState::Authenticated:   return "Authenticated";
        case AccountState::Refreshing:      return "Refreshing";
        case AccountState::LoggedOutKicked: return "LoggedOutKicked";
    }
    return "?";
}

const char* toString(ConnectionState s)
{
    switch (s) {
        case ConnectionState::Disconnected: return "Disconnected";
        case ConnectionState::Connecting:   return "Connecting";
        case ConnectionState::Connected:    return "Connected";
        case ConnectionState::Reconnecting: return "Reconnecting";
    }
    return "?";
}

const char* toString(AuthError e)
{
    switch (e) {
        case AuthError::None:                return "None";
        case AuthError::NetworkUnreachable:  return "NetworkUnreachable";
        case AuthError::InvalidCredentials:  return "InvalidCredentials";
        case AuthError::TokenExpired:        return "TokenExpired";
        case AuthError::TokenRevoked:        return "TokenRevoked";
        case AuthError::DeviceProofFailed:   return "DeviceProofFailed";
        case AuthError::KickedByOtherDevice: return "KickedByOtherDevice";
        case AuthError::OperationInProgress: return "OperationInProgress";
        case AuthError::OperationSuperseded: return "OperationSuperseded";
        case AuthError::ServerRejected:      return "ServerRejected";
        case AuthError::Internal:            return "Internal";
    }
    return "?";
}

} // namespace account
} // namespace im
