// BridgeCore: everything the bridge does that doesn't need the sim.
//
// The module is split in two so most of it can be tested on a desktop:
//   BridgeCore.h/.cpp    parsing, validation, replies, chunking, the L-var diff. Plain C++17, no MSFS headers.
//   ClikstpBridge.cpp  SimApi on top of the real MSFS calls, plus module_init/module_deinit. Kept thin.
//   tests/               a FakeSim that behaves like the MSFS API, unit tests, and a fuzz target.
//
// The core talks to the sim only through SimApi. Each method names the MSFS call it stands for, so the tests double
// as notes on how that API behaves.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace clikstp
{
    constexpr const char* kVersion = "6";
    constexpr size_t kCommandSize = 1024;    // Clikstp.Bridge.Command
    constexpr size_t kResponseSize = 8192;   // Clikstp.Bridge.Response (SIMCONNECT_CLIENTDATA_MAX_SIZE)
    constexpr int kInvalidId = -1;           // FS_VAR_INVALID_ID

    // The sim, as the core sees it. The real one is in ClikstpBridge.cpp; tests use tests/FakeSim.h.
    class SimApi
    {
    public:
        virtual ~SimApi() = default;

        // fsVarsGetLVarName(id, buffer, size): the name of L-var `id`, or false if there's none with that id.
        virtual bool LVarName(int id, std::string& name) = 0;
        // fsVarsGetLVarId(name): the id, or kInvalidId. Never creates the L-var (fsVarsRegisterLVar would).
        virtual int FindLVar(const std::string& name) = 0;
        // fsVarsLVarGet / fsVarsLVarSet with the "number" unit. False means the call returned an error.
        virtual bool GetLVar(int id, double& value) = 0;
        virtual bool SetLVar(int id, double value) = 0;

        // fsVarsGetBVarId(name, true) on the user aircraft: the id, or kInvalidId when it has no such B: event.
        virtual int FindBVar(const std::string& name) = 0;
        // fsVarsBVarCall(id, no params): 0 (FS_VAR_ERROR_NONE) or the FsVarError code.
        virtual unsigned CallBVar(int id) = 0;

        // execute_calculator_code(code, &float, &int, &string): false if the code didn't run.
        virtual bool Calc(const std::string& code, double& result) = 0;

        // SimConnect_SetClientData on the response area. Always called with exactly kResponseSize bytes, the text
        // NUL-terminated and the rest zeroed, so a short reply never carries the tail of a longer, older one.
        virtual void Send(const char* buffer, size_t size) = 0;
        // The dev mode console.
        virtual void Log(const std::string& text) = 0;
    };

    class Bridge
    {
    public:
        explicit Bridge(SimApi& sim) : sim_(sim) {}

        // A raw client data packet: at most `size` bytes, maybe unterminated, maybe garbage. Never trusts it.
        void HandlePacket(const char* data, size_t size);

        // One command: "<id> <verb> [args]". "-" as the id asks for no reply (errors then go to the log).
        void Handle(const std::string& text);

        // Forget the diff baseline (aircraft change, module_deinit).
        void Reset() { snapshot_.clear(); }

        // Guards for the L-var walk: ids aren't promised to be contiguous, so the walk tolerates kMaxIdGap misses in a
        // row; kMaxLVarId stops it even if a broken sim claimed every id exists.
        static constexpr int kMaxIdGap = 16;
        static constexpr int kMaxLVarId = 1 << 20;

    private:
        void Reply(const std::string& id, const std::string& status, const std::string& payload = "");
        void SendText(const std::string& text);

        void Calc(const std::string& id, const std::string& code);
        void ListLVars(const std::string& id);
        void Diff(const std::string& id);
        void Get(const std::string& id, const std::vector<std::string>& a);
        void GetMany(const std::string& id, const std::string& names);
        void Set(const std::string& id, const std::vector<std::string>& a);
        void Step(const std::string& id, const std::vector<std::string>& a);
        void HVar(const std::string& id, const std::vector<std::string>& a);
        void BVar(const std::string& id, const std::vector<std::string>& a);
        bool FindLVar(const std::string& id, const std::string& raw, int& out);

        template <typename F> void ForEachLVar(F f);

        friend class Chunker;
        SimApi& sim_;
        std::vector<double> snapshot_;   // L-var values at the last diff; index = id; NaN = not seen yet
    };

    // Helpers, public for the tests.
    std::vector<std::string> Split(const std::string& text, size_t maxParts);
    bool ParseNumber(const std::string& text, double& out);
    std::string Number(double v);
    std::string LVarName(std::string name);
    std::string Unprefixed(std::string name, char letter);
}
