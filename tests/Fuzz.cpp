// Fuzz target for the bridge's command handling: libFuzzer feeds it millions of made-up packets, looking for one
// that crashes, hangs or trips AddressSanitizer. Any SimConnect client can write the command area, so this is the
// module's attack surface.
//
// Build and run (clang): tests/run-tests.sh fuzz     or by hand:
//   clang++ -std=c++17 -g -fsanitize=fuzzer,address,undefined tests/Fuzz.cpp src/BridgeCore.cpp -o fuzz
//   ./fuzz -max_total_time=60 tests/corpus
// A crash leaves crash-<hash> with the input; replay it with ./fuzz crash-<hash>.

#include "FakeSim.h"

#include <cstddef>
#include <cstdint>

using namespace clikstp;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // A small aircraft that every command can find something in.
    static test::FakeSim* sim = nullptr;
    static Bridge* bridge = nullptr;
    if (!sim)
    {
        sim = new test::FakeSim();
        sim->AddLVar("A", 0.5);
        sim->AddLVar("BARO", 29.92);
        sim->SkipIds(3);
        sim->AddLVar("C", 1);
        sim->AddBVar("AS530_CLR_Push");
        bridge = new Bridge(*sim);
    }

    bridge->HandlePacket(reinterpret_cast<const char*>(data), size);

    // Keep memory flat across millions of runs.
    if (sim->sent.size() > 1000) sim->sent.clear();
    if (sim->logs.size() > 1000) sim->logs.clear();
    if (sim->calcs.size() > 1000) sim->calcs.clear();
    if (sim->calledBVars.size() > 1000) sim->calledBVars.clear();
    return 0;
}
