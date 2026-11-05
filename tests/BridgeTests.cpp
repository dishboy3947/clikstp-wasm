// Unit tests for BridgeCore, against FakeSim. No framework: CHECK prints failures, main returns non-zero on any.
//
// Build and run: tests/run-tests.sh (Linux, macOS, WSL) or tests\run-tests.cmd (Windows). Both build with
// AddressSanitizer, so reading past a buffer or using freed memory fails the run loudly, with a line number,
// instead of crashing the sim later.
//
// See the main clikstp app repo at docs/bridge-protocol.md as it contains what the client sends, what comes back.

#include "FakeSim.h"
#include "../src/BridgeCore.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace clikstp;
using clikstp::test::FakeSim;

static int g_checks = 0, g_failed = 0;

#define CHECK(cond, what)                                                         \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) { ++g_failed; printf("  FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__); } \
    } while (0)

#define SECTION(name) printf("%s\n", name)

static bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

int main()
{
    SECTION("Helpers");
    {
        auto p = Split("a  b c d", 3);
        CHECK(p.size() == 3 && p[0] == "a" && p[1] == "b" && p[2] == "c d", "Split keeps the rest of the line in the last part");
        CHECK(Split("   ", 3).empty() && Split("", 3).empty() && Split("x", 0).empty(), "Split on blanks, empty, zero parts");
        
        double v;
        CHECK(ParseNumber("1.5", v) && v == 1.5 && ParseNumber("-2", v) && v == -2, "ParseNumber accepts plain numbers");
        CHECK(!ParseNumber("1.5x", v) && !ParseNumber("", v) && !ParseNumber("nan", v) && !ParseNumber("inf", v),
              "ParseNumber refuses trailing junk, empty, NaN and infinity");
        CHECK(LVarName("L:FOO, number") == "FOO" && LVarName("l:BAR") == "BAR" && LVarName("BAZ  ") == "BAZ",
              "LVarName strips L:, a unit suffix and trailing spaces");
        CHECK(Unprefixed("B:AS530_CLR_Push", 'B') == "AS530_CLR_Push" && Unprefixed("h:X", 'H') == "X"
              && Unprefixed("AS530_CLR", 'B') == "AS530_CLR", "Unprefixed strips one letter prefix, either case");
        CHECK(Number(29.92) == "29.92" && Number(1) == "1" && Number(0.1 + 0.2) == "0.3", "Number prints short, 10 digits");
    }

    SECTION("ping and framing");
    {
        FakeSim sim;
        Bridge b(sim);
        b.Handle("7 ping");
        CHECK(sim.Last() == std::string("7 ok clikstp-bridge ") + kVersion, "ping answers with the version");
        
        b.Handle("8 ping\r\n");
        CHECK(StartsWith(sim.Last(), "8 ok"), "trailing CR/LF is ignored");
        
        size_t before = sim.sent.size();
        b.Handle("");
        b.Handle("   ");
        b.Handle("lonely");
        CHECK(sim.sent.size() == before && !sim.logs.empty(), "empty and one-word commands get no reply; the last is logged");
        
        b.Handle("9 fly");
        CHECK(sim.Last() == "9 err unknown command fly", "unknown verb is an error, not silence");
        CHECK(sim.badSends == 0, "every reply was a full, terminated 8 KB area");
    }

    SECTION("Fire-and-forget (id -)");
    {
        FakeSim sim;
        sim.AddLVar("A", 1);
        Bridge b(sim);
        b.Handle("- set A 2");
        CHECK(sim.sent.empty() && sim.Value("A") == 2, "id - does the work and sends nothing");
        
        b.Handle("- set NOPE 2");
        CHECK(sim.sent.empty() && !sim.logs.empty() && sim.logs.back() == "error: no L-var named NOPE",
              "id - errors go to the dev console log");
       
        b.Handle("- lvars");
        CHECK(sim.sent.empty(), "id - list commands send no parts either");
    }

    SECTION("get / set / step");
    {
        FakeSim sim;
        sim.AddLVar("DIMMER", 0.5);
        sim.AddLVar("BARO", 29.92);
        Bridge b(sim);

        b.Handle("1 get L:DIMMER");
        CHECK(sim.Last() == "1 ok 0.5", "get reads the value; L: prefix is fine");
        
        b.Handle("2 get MISSING");
        CHECK(sim.Last() == "2 err no L-var named MISSING", "get never creates an L-var");
        
        b.Handle("3 get");
        CHECK(sim.Last() == "3 err usage: get NAME", "get without a name");

        b.Handle("4 set DIMMER 0.75");
        CHECK(sim.Last() == "4 ok 0.75" && sim.Value("DIMMER") == 0.75, "set writes and echoes");
       
        b.Handle("5 set DIMMER lots");
        CHECK(sim.Last() == "5 err usage: set NAME VALUE" && sim.Value("DIMMER") == 0.75, "set with a bad number changes nothing");

        b.Handle("6 step DIMMER 0.1");
        CHECK(sim.Last() == "6 ok 0.85", "step adds within 0..1 by default");
        
        b.Handle("7 step DIMMER 0.5");
        CHECK(sim.Last() == "7 ok 1", "step clamps at the top");
       
        b.Handle("8 step DIMMER -5");
        CHECK(sim.Last() == "8 ok 0", "step clamps at the bottom");
       
        b.Handle("9 step BARO 0.01 28 31");
        CHECK(sim.Last() == "9 ok 29.93", "step with a range, rounded to 4 decimals");
        
        b.Handle("10 step BARO 0.01 31 28");
        CHECK(StartsWith(sim.Last(), "10 err usage"), "step refuses min >= max");
        
        b.Handle("11 step BARO 0.01 28");
        CHECK(StartsWith(sim.Last(), "11 err usage"), "step refuses a min without a max");

        sim.BreakReads("BARO");
        b.Handle("12 get BARO");
        CHECK(sim.Last() == "12 err could not read BARO", "a failing read is reported");
    }

    SECTION("getmany (the app's L-var poll)");
    {
        FakeSim sim;
        sim.AddLVar("A", 1.5);
        sim.AddLVar("B", 0);
        sim.AddLVar("BROKEN", 3);
        sim.BreakReads("BROKEN");
        Bridge b(sim);
        b.Handle("1 getmany A L:B MISSING BROKEN");
        CHECK(sim.sent.size() == 2 && sim.sent[0] == "1 part 0\nA\t1.5\nB\t0\nMISSING\t-\nBROKEN\t-\n" && sim.Last() == "1 ok 4",
              "getmany: values in the order asked, - for missing or unreadable");
        
        b.Handle("2 getmany");
        CHECK(sim.Last() == "2 err usage: getmany NAME [NAME...]", "getmany without names");
        
        size_t before = sim.sent.size();
        b.Handle("- getmany A B");
        CHECK(sim.sent.size() == before, "getmany with id - sends nothing");
    }

    SECTION("lvars (chunked list)");
    {
        FakeSim sim;
        sim.AddLVar("A", 1);
        sim.SkipIds(5);                 // a gap smaller than the walk tolerates
        sim.AddLVar("B", 2.5);
        sim.BreakReads("B");
        Bridge b(sim);
        b.Handle("4 lvars");
        CHECK(sim.sent.size() == 2 && sim.sent[0] == "4 part 0\nA\t1\nB\tnan\n" && sim.sent[1] == "4 ok 2",
              "lvars: one part with every L-var across a gap, unreadable ones as nan, then ok COUNT");

        FakeSim many;
        for (int i = 0; i < 2000; ++i) many.AddLVar("SOME_LONG_LVAR_NAME_" + std::to_string(i), i);
        Bridge bm(many);
        bm.Handle("5 lvars");
        bool partsOk = many.sent.size() > 2 && many.Last() == "5 ok 2000";
        int lines = 0;
        for (size_t i = 0; i + 1 < many.sent.size(); ++i)
        {
            const std::string& part = many.sent[i];
            partsOk = partsOk && StartsWith(part, "5 part " + std::to_string(i) + "\n") && part.size() < kResponseSize && part.back() == '\n';
            for (char c : part) if (c == '\n') ++lines;
        }
        CHECK(partsOk, "a long list splits into numbered parts that each fit the area");
        CHECK(lines - (int)(many.sent.size() - 1) == 2000, "no line is lost across parts");
        CHECK(many.badSends == 0, "parts are full, terminated areas");

        FakeSim huge;
        huge.AddLVar(std::string(9000, 'N'), 1);
        huge.AddLVar("AFTER", 2);
        Bridge bh(huge);
        bh.Handle("6 lvars");
        CHECK(huge.Last() == "6 ok 2" && huge.sent.size() == 3 && huge.sent[0].size() < kResponseSize,
              "a name longer than the whole area is cut to fit, and the list carries on");

        FakeSim liar;
        liar.claimEveryId = true;
        Bridge bl(liar);
        bl.Handle("7 lvars");
        CHECK(liar.nameCalls == Bridge::kMaxLVarId && StartsWith(liar.Last(), "7 ok"),
              "a sim that claims every id exists can't make the walk run forever");
    }

    SECTION("diff");
    {
        FakeSim sim;
        sim.AddLVar("A", 0);
        sim.AddLVar("B", 5);
        Bridge b(sim);
        b.Handle("1 diff");
        CHECK(sim.Last() == "1 ok 0" && sim.sent.size() == 1, "the first diff only takes a baseline");
        
        sim.SetValue("A", 1);
        b.Handle("2 diff");
        CHECK(sim.sent[sim.sent.size() - 2] == "2 part 0\nA\t0\t1\n" && sim.Last() == "2 ok 1", "a change is reported as was/now");
       
        b.Handle("3 diff");
        CHECK(sim.Last() == "3 ok 0", "no change, nothing reported");
       
        sim.AddLVar("C", 7);
        b.Handle("4 diff");
        CHECK(sim.Last() == "4 ok 0", "an L-var that appears is baselined quietly");
        
        sim.SetValue("C", 8);
        b.Handle("5 diff");
        CHECK(sim.Last() == "5 ok 1", "and reported once it changes");
        
        b.Reset();
        sim.SetValue("A", 2);
        b.Handle("6 diff");
        CHECK(sim.Last() == "6 ok 0", "after Reset the next diff is a baseline again");
    }

    SECTION("bvar");
    {
        FakeSim sim;
        sim.AddBVar("AS5_a2amod_1_Knob_Button_Push");
        sim.AddBVar("AS5_a2amod_1_Knob_Button_Release");
        sim.AddBVar("BROKEN", 4);   // FS_VAR_ERROR_INVALID_ARGS
        Bridge b(sim);
        b.Handle("1 bvar B:AS5_a2amod_1_Knob_Button_Push");
        b.Handle("2 bvar AS5_a2amod_1_Knob_Button_Release");
        CHECK(sim.calledBVars.size() == 2 && sim.calledBVars[0] == "AS5_a2amod_1_Knob_Button_Push" && sim.Last() == "2 ok",
              "bvar calls the B: event, with or without the B: prefix");
        
        b.Handle("3 bvar AS530_CLR_Bogus");
        CHECK(sim.Last() == "3 err no callable B: event named AS530_CLR_Bogus", "an unknown B: event is an error");
        
        b.Handle("4 bvar BROKEN");
        CHECK(sim.Last() == "4 err B: event BROKEN failed (4)", "a failing call reports the FsVarError code");
    }

    SECTION("hvar and calc");
    {
        FakeSim sim;
        Bridge b(sim);
        b.Handle("1 hvar H:GTN750_HomePush");
        CHECK(sim.calcs.size() == 1 && sim.calcs[0] == "(>H:GTN750_HomePush)" && sim.Last() == "1 ok 0",
              "hvar fires the H-event as calculator code");
       
        b.Handle("2 hvar FOO) (>L:BAR");
        CHECK(StartsWith(sim.Last(), "2 err bad H-event name") && sim.calcs.size() == 1,
              "hvar refuses a name that would smuggle in more calculator code");
        
        sim.calcResult = 42;
        b.Handle("3 calc (L:FOO) 1 +");
        CHECK(sim.calcs.back() == "(L:FOO) 1 +" && sim.Last() == "3 ok 42", "calc passes the whole rest of the line, spaces and all");
        
        sim.calcFails = true;
        b.Handle("4 calc ((((");
        CHECK(sim.Last() == "4 err calculator code failed: ((((", "calc reports code that didn't run");
       
        b.Handle("5 calc");
        CHECK(sim.Last() == "5 err usage: calc CODE", "calc without code");
    }

    SECTION("Raw packets (what SimConnect hands the module)");
    {
        FakeSim sim;
        Bridge b(sim);
        char packet[kCommandSize + 50];
        memset(packet, 'x', sizeof packet);   // no terminator anywhere
        b.HandlePacket(packet, sizeof packet);
        CHECK(sim.badSends == 0, "an unterminated packet longer than the area is cut and terminated");

        const char ping[] = "5 ping\0garbage after the terminator";
        b.HandlePacket(ping, sizeof ping);
        CHECK(StartsWith(sim.Last(), "5 ok"), "text after the first NUL is ignored");

        b.HandlePacket(nullptr, 100);
        b.HandlePacket(ping, 0);
        CHECK(true, "null and empty packets don't crash");
    }

    printf("\n%s %d checks, %d failed.\n", g_failed ? "FAILED:" : "All passed:", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
