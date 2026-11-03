// ClikstpBridge: a small standalone WASM module for MSFS 2024.
//
// It gives CLIKSTP (and the recon tool) what SimConnect alone can't:
//   * call B: events (input event bindings such as a G5 knob push) and fire H-events
//   * run calculator code (RPN)
//   * list the loaded aircraft's L-vars with their values
//   * get, set and step an L-var (the step happens here, in one frame, so there is no read-back lag)
//   * report which L-vars changed since the last ask (for recon)
//
// Transport: two SimConnect client data areas.
//   Clikstp.Bridge.Command   (1 KB)  written by clients, read here
//   Clikstp.Bridge.Response  (8 KB)  written here, read by clients
// Both hold one NUL-terminated ASCII text message. The protocol is in docs/bridge-protocol.md.
//
// This file is only the sim layer: SimApi on top of the real MSFS calls, and module_init/module_deinit. Everything
// else is in BridgeCore.cpp, which tests/ builds and tests on a desktop. Keep this file thin: what's here is only
// tested in the sim.
//
// Uses the MSFS 2024 API (MSFS_Vars.h). Calculator code still goes through the legacy gauges.h call, which the
// 2024 SDK marks deprecated but has no replacement for.

#include <MSFS/MSFS.h>
#include <MSFS/MSFS_Vars.h>
#include <MSFS/Legacy/gauges.h>
#include <SimConnect.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BridgeCore.h"

namespace
{
    using namespace clikstp;

    constexpr const char* kCommandArea = "Clikstp.Bridge.Command";
    constexpr const char* kResponseArea = "Clikstp.Bridge.Response";

    enum ClientDataId : SIMCONNECT_CLIENT_DATA_ID { CD_COMMAND = 0x4D47, CD_RESPONSE = 0x4D48 };
    enum DefineId : SIMCONNECT_CLIENT_DATA_DEFINITION_ID { DEF_COMMAND = 0x4D47, DEF_RESPONSE = 0x4D48 };
    enum RequestId : SIMCONNECT_DATA_REQUEST_ID { REQ_COMMAND = 0x4D47 };
    enum EventId : SIMCONNECT_CLIENT_EVENT_ID { EV_FLIGHT_LOADED = 0x4D47, EV_SIM_START = 0x4D48 };

    HANDLE g_sim = 0;
    FsUnitId g_number = FS_INVALID_UNIT;     // the "number" unit, for L-var reads and writes

    FsVarParamArray NoParams()
    {
        FsVarParamArray p;
        memset(&p, 0, sizeof p);
        return p;
    }

    void LogLine(const char* text)
    {
        // Both streams: which one the dev console shows isn't documented.
        fprintf(stderr, "[ClikstpBridge] %s\n", text);
        printf("[ClikstpBridge] %s\n", text);
    }

    class MsfsSim final : public SimApi
    {
    public:
        bool LVarName(int id, std::string& name) override
        {
            char buffer[256];
            if (fsVarsGetLVarName(id, buffer, sizeof buffer) != FS_VAR_ERROR_NONE) return false;
            buffer[sizeof buffer - 1] = '\0';
            name = buffer;
            return true;
        }

        int FindLVar(const std::string& name) override { return fsVarsGetLVarId(name.c_str()); }

        bool GetLVar(int id, double& value) override { return fsVarsLVarGet(id, g_number, &value) == FS_VAR_ERROR_NONE; }

        bool SetLVar(int id, double value) override { return fsVarsLVarSet(id, g_number, value) == FS_VAR_ERROR_NONE; }

        int FindBVar(const std::string& name) override { return fsVarsGetBVarId(name.c_str(), true); }

        unsigned CallBVar(int id) override { return (unsigned)fsVarsBVarCall(id, NoParams()); }

        bool Calc(const std::string& code, double& result) override
        {
            FLOAT64 f = 0;
            SINT32 i = 0;
            PCSTRINGZ s = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            BOOL ok = execute_calculator_code(code.c_str(), &f, &i, &s);
#pragma clang diagnostic pop
            result = f;
            return ok;
        }

        void Send(const char* buffer, size_t size) override
        {
            SimConnect_SetClientData(g_sim, CD_RESPONSE, DEF_RESPONSE, SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 0,
                (DWORD)size, (void*)buffer);
        }

        void Log(const std::string& text) override { LogLine(text.c_str()); }
    };

    // Made in module_init rather than as static objects, so nothing depends on the sim running global constructors.
    MsfsSim* g_api = nullptr;
    Bridge* g_bridge = nullptr;

    // A marker any SimConnect client can read (L:CLIKSTP_BRIDGE_VERSION): the version once running, -1 if
    // SimConnect couldn't be opened, 0 (the default) if the module never started. CLIKSTP reads it when the
    // bridge doesn't answer, to tell "not loaded" from "loaded but not answering".
    void SetMarker(double value)
    {
        FsLVarId v = fsVarsRegisterLVar("CLIKSTP_BRIDGE_VERSION");
        if (v != FS_VAR_INVALID_ID) fsVarsLVarSet(v, g_number, value);
    }

    // Loading a flight resets L-vars, the bridge's own included (seen on the bench: the marker read 0 while the
    // bridge answered). Register and set them again after each load.
    void SetOwnLVars()
    {
        fsVarsRegisterLVar("CLIKSTP_BRIDGE_TEST");   // scratch L-var for the live self-test (ClikstpRecon "selftest")
        SetMarker(atof(kVersion));
    }

    void CALLBACK Dispatch(SIMCONNECT_RECV* data, DWORD size, void*)
    {
        if (!data) return;
        switch (data->dwID)
        {
        case SIMCONNECT_RECV_ID_CLIENT_DATA:
        {
            auto* cd = static_cast<SIMCONNECT_RECV_CLIENT_DATA*>(data);
            if (cd->dwRequestID != REQ_COMMAND) break;
            // Hand the core no more than the packet holds; it copies and terminates.
            const char* start = reinterpret_cast<const char*>(&cd->dwData);
            size_t offset = (size_t)(start - reinterpret_cast<const char*>(data));
            if (g_bridge) g_bridge->HandlePacket(start, size > offset ? size - offset : 0);
            break;
        }
        case SIMCONNECT_RECV_ID_EVENT:
        case SIMCONNECT_RECV_ID_EVENT_FILENAME:
        {
            auto* ev = static_cast<SIMCONNECT_RECV_EVENT*>(data);
            if (ev->uEventID == EV_FLIGHT_LOADED || ev->uEventID == EV_SIM_START) SetOwnLVars();
            break;
        }
        case SIMCONNECT_RECV_ID_EXCEPTION:
        {
            auto* ex = static_cast<SIMCONNECT_RECV_EXCEPTION*>(data);
            char msg[96];
            snprintf(msg, sizeof msg, "SimConnect exception %u (sendID %u)", (unsigned)ex->dwException, (unsigned)ex->dwSendID);
            LogLine(msg);
            break;
        }
        default:
            break;
        }
    }
}

extern "C" MSFS_CALLBACK void module_init(void)
{
    g_number = fsVarsGetUnitId("number");
    LogLine("starting");
    if (!g_api) g_api = new MsfsSim();
    if (!g_bridge) g_bridge = new Bridge(*g_api);

    if (FAILED(SimConnect_Open(&g_sim, "ClikstpBridge", nullptr, 0, 0, 0)))
    {
        LogLine("could not open SimConnect; bridge disabled");
        SetMarker(-1);
        return;
    }

    SimConnect_MapClientDataNameToID(g_sim, kCommandArea, CD_COMMAND);
    SimConnect_MapClientDataNameToID(g_sim, kResponseArea, CD_RESPONSE);
    SimConnect_CreateClientData(g_sim, CD_COMMAND, (DWORD)kCommandSize, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);
    SimConnect_CreateClientData(g_sim, CD_RESPONSE, (DWORD)kResponseSize, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);
    SimConnect_AddToClientDataDefinition(g_sim, DEF_COMMAND, 0, (DWORD)kCommandSize, 0, SIMCONNECT_UNUSED);
    SimConnect_AddToClientDataDefinition(g_sim, DEF_RESPONSE, 0, (DWORD)kResponseSize, 0, SIMCONNECT_UNUSED);
    SimConnect_RequestClientData(g_sim, CD_COMMAND, REQ_COMMAND, DEF_COMMAND,
        SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET, SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    SimConnect_SubscribeToSystemEvent(g_sim, EV_FLIGHT_LOADED, "FlightLoaded");
    SimConnect_SubscribeToSystemEvent(g_sim, EV_SIM_START, "SimStart");
    SimConnect_CallDispatch(g_sim, Dispatch, nullptr);

    SetOwnLVars();
    LogLine("ready");
}

extern "C" MSFS_CALLBACK void module_deinit(void)
{
    if (g_sim) SimConnect_Close(g_sim);
    g_sim = 0;
    if (g_bridge) g_bridge->Reset();
}
