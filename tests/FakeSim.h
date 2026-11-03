// FakeSim: a stand-in for the MSFS API, for testing BridgeCore on a desktop.
//
// It's also a set of notes on how the real calls behave, as far as the bench has shown. Where the behaviour is a
// guess rather than something seen in the sim, the comment says so. When the sim surprises us, fix the fake first,
// then the test, then the code.

#pragma once

#include "../src/BridgeCore.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace clikstp::test
{
    class FakeSim final : public SimApi
    {
    public:
        // --- L-vars -----------------------------------------------------------------------------------------
        //
        // In the sim every L-var has an id (an int) and a name. Ids start at 0 and are handed out as L-vars get
        // registered: by the aircraft's gauges and behaviors as it loads, or by any module calling
        // fsVarsRegisterLVar. They're not promised to be contiguous, which is why the bridge's walk tolerates gaps.
        //
        // L-vars outlive the aircraft: load a second aircraft and the first one's L-vars are still there, with
        // their last values (seen on the bench: stale XMLVAR_GPS_DISABLED_* values across aircraft).
        //
        // fsVarsGetLVarId(name) only finds; it never creates. Names are matched case-insensitively in the sim
        // (a guess; the fake matches exactly so the tests stay honest about spelling).

        // Adds an L-var at the next id, like fsVarsRegisterLVar. Returns its id.
        int AddLVar(const std::string& name, double value = 0)
        {
            int id = nextId_++;
            lvars_[id] = { name, value };
            return id;
        }

        // Leaves `count` unused ids, to mimic a gap in the id sequence.
        void SkipIds(int count) { nextId_ += count; }

        // Makes reads of this L-var fail, like fsVarsLVarGet returning an error.
        void BreakReads(const std::string& name) { brokenReads_.push_back(name); }

        double Value(const std::string& name) const
        {
            for (auto& [id, v] : lvars_) if (v.name == name) return v.value;
            return NAN;
        }

        void SetValue(const std::string& name, double value)
        {
            for (auto& [id, v] : lvars_) if (v.name == name) v.value = value;
        }

        bool LVarName(int id, std::string& name) override
        {
            ++nameCalls;
            if (claimEveryId) { name = "X" + std::to_string(id); return true; }
            auto it = lvars_.find(id);
            if (it == lvars_.end()) return false;
            name = it->second.name;
            return true;
        }

        int FindLVar(const std::string& name) override
        {
            for (auto& [id, v] : lvars_) if (v.name == name) return id;
            return kInvalidId;
        }

        bool GetLVar(int id, double& value) override
        {
            auto it = lvars_.find(id);
            if (it == lvars_.end()) return claimEveryId ? (value = 0, true) : false;
            for (auto& b : brokenReads_) if (b == it->second.name) return false;
            value = it->second.value;
            return true;
        }

        bool SetLVar(int id, double value) override
        {
            auto it = lvars_.find(id);
            if (it == lvars_.end()) return false;
            it->second.value = value;
            return true;
        }

        // --- B: events ----------------------------------------------------------------------------------------
        //
        // B: events are an aircraft's input event bindings: an input event NAME (listed by SimConnect) has
        // bindings like NAME_Push, NAME_Release, NAME_Set. fsVarsGetBVarId(name, true) looks one up on the user
        // aircraft. Seen on the bench: the A2A G5 knob push needs _Push then _Release in quick succession;
        // nothing happens on _Push alone. Confirmed by ClikstpRecon's selftest (2026-10-26): an unknown name
        // (AS530_CLR_Bogus) gets FS_VAR_INVALID_ID, so the fake returns kInvalidId too. That also means B: events
        // could be found by trying likely names (input event + _Push, _Release, ...), since misses are reported.

        void AddBVar(const std::string& name, unsigned resultCode = 0) { bvars_[name] = resultCode; }

        int FindBVar(const std::string& name) override
        {
            int i = 0;
            for (auto& [n, code] : bvars_) { if (n == name) return 1000 + i; ++i; }
            return kInvalidId;
        }

        unsigned CallBVar(int id) override
        {
            int i = 0;
            for (auto& [n, code] : bvars_)
            {
                if (1000 + i == id) { calledBVars.push_back(n); return code; }
                ++i;
            }
            return 0xffffffffu;   // FS_VAR_ERROR_FAIL
        }

        // --- Calculator code ----------------------------------------------------------------------------------
        //
        // execute_calculator_code runs RPN ("(>H:GTN750_HomePush)", "(L:FOO) 1 +") and returns a float result.
        // The fake doesn't evaluate anything: it records the code and answers with calcResult, or fails when
        // calcFails is set. Seen on the bench: firing H-events this way works where fsEventsHEventCall didn't.

        bool Calc(const std::string& code, double& result) override
        {
            calcs.push_back(code);
            if (calcFails) return false;
            result = calcResult;
            return true;
        }

        // --- Transport ----------------------------------------------------------------------------------------
        //
        // The real Send is SimConnect_SetClientData on the response area; every client that asked for the area
        // gets the whole 8 KB. The fake keeps each message as text, and checks the bridge kept its promise of
        // always sending the full, terminated area.

        void Send(const char* buffer, size_t size) override
        {
            if (size != kResponseSize || buffer[size - 1] != '\0') badSends++;
            sent.emplace_back(buffer);   // up to the first NUL
        }

        void Log(const std::string& text) override { logs.push_back(text); }

        // --- What happened ------------------------------------------------------------------------------------

        std::vector<std::string> sent, logs, calcs, calledBVars;
        int badSends = 0;
        int nameCalls = 0;
        bool calcFails = false;
        double calcResult = 0;
        bool claimEveryId = false;   // a broken sim that says every id exists

        std::string Last() const { return sent.empty() ? "" : sent.back(); }

    private:
        struct LVar { std::string name; double value; };
        std::map<int, LVar> lvars_;
        std::map<std::string, unsigned> bvars_;
        std::vector<std::string> brokenReads_;
        int nextId_ = 0;
    };
}
