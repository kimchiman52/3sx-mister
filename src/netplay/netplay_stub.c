#include "netplay/netplay.h"
#include "netplay/netplay_nav.h"

#include <string.h>

void Netplay_SetParams(int player, const char* ip) {
    (void)player;
    (void)ip;
}

bool Netplay_IsRemoteIpSet(void) {
    return false;
}

void NetplayNav_Arm(void) {
}

void NetplayNav_Tick(void) {
}

bool NetplayNav_IsActive(void) {
    return false;
}

void NetplayNav_Reset(void) {
}

void Netplay_BeginDirectP2P() {
}

void Netplay_TickDirectP2P() {
}

void Netplay_SetStunSocket(struct NET_DatagramSocket* socket) {
    (void)socket;
}

void Netplay_SetSessionTeardownCallback(void (*cb)(void)) {
    (void)cb;
}

void Netplay_LogConnectEvent(const char* line) {
    (void)line;
}

void Netplay_LogSinkInit(void) {
}

void Netplay_LogSinkShutdown(void) {
}

void Netplay_LogConnectEventMT(const char* line) {
    (void)line;
}

#ifdef NETPLAY_TEST_HOOKS
/* #44: NETPLAY_TEST_HOOKS is an independent CMake option
 * (CMakeLists.txt:98, 296-297) and is not implied by ENABLE_NETPLAY, so a
 * hooks-on / netplay-off configuration must still link. */
void Netplay_TestHook_LogPrune(const char* dir) {
    (void)dir;
}

bool Netplay_TestHook_SessionLogPath(char* out, size_t cap) {
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    return false;
}

void Netplay_TestHook_ReportDir(const char* dir) {
    (void)dir;
}

void Netplay_TestHook_HeartbeatEnqueue(const char* line) {
    (void)line;
}

void Netplay_TestHook_HeartbeatDrain(void) {
}
#endif

void Netplay_Run() {
}

NetplaySessionState Netplay_GetSessionState() {
    return NETPLAY_SESSION_IDLE;
}

const char* Netplay_GetConnectStatusText(void) {
    return "";
}

void Netplay_HandleMenuExit() {
}

bool Netplay_ArmAllowed(void) {
    return false;
}

void Netplay_RefuseArm(void) {
}

void Netplay_GetNetworkStats(NetworkStats* stats) {
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
}

void Netplay_FlushDiagnostics(void) {
}
