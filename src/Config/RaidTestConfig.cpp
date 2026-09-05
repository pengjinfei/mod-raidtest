#include "RaidTestConfig.h"
#include "Config.h"

RaidTestConfig& RaidTestConfig::instance()
{
    static RaidTestConfig instance;
    return instance;
}

void RaidTestConfig::Initialize()
{
    _enabled = sConfigMgr->GetOption<bool>("RaidTest.Enabled", true);
    _accountPrefix = sConfigMgr->GetOption<std::string>("RaidTest.AccountPrefix", "raidtest");
    _partySize = sConfigMgr->GetOption<uint8>("RaidTest.PartySize", 25);
    _defaultAttempts = sConfigMgr->GetOption<uint32>("RaidTest.DefaultAttempts", 5);
    _attemptTimeoutSeconds = sConfigMgr->GetOption<uint32>("RaidTest.AttemptTimeout", 300);
    _forceRecreateOnRun = sConfigMgr->GetOption<bool>("RaidTest.ForceRecreateOnRun", false);
    _logLevel = sConfigMgr->GetOption<uint8>("RaidTest.LogLevel", 1);
}