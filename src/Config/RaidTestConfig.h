#ifndef PLAYERBOTS_RAIDTEST_CONFIG_H
#define PLAYERBOTS_RAIDTEST_CONFIG_H

#include "Define.h"
#include <string>

class RaidTestConfig
{
public:
    static RaidTestConfig& instance();
    void Initialize();
    bool Enabled() const { return _enabled; }
    std::string const& AccountPrefix() const { return _accountPrefix; }
    uint8 PartySize() const { return _partySize; }
    uint32 DefaultAttempts() const { return _defaultAttempts; }
    uint32 AttemptTimeoutSeconds() const { return _attemptTimeoutSeconds; }
    bool ForceRecreateOnRun() const { return _forceRecreateOnRun; }
    uint8 LogLevel() const { return _logLevel; }
private:
    bool _enabled{true};
    std::string _accountPrefix{"raidtest"};
    uint8 _partySize{10};
    uint32 _defaultAttempts{5};
    uint32 _attemptTimeoutSeconds{300};
    bool _forceRecreateOnRun{false};
    uint8 _logLevel{1};
};

#endif