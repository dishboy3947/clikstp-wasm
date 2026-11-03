# ClikstpBridge (WASM module)

A small standalone WASM module that runs inside MSFS 2024 and gives Clikstp what SimConnect alone can't:

- **B: events**, including bindings SimConnect's input events don't expose (the G5 knob push), and **H-events**
- **calculator code** (RPN), for anything else
- **L-var listing** with values, for the recon collection and for `requires: ["L:NAME"]`
- **get / set / step** an L-var; the step is read-add-clamp-write inside one sim frame
- **getmany**: several L-vars in one round trip (the app's L-var poll)
- **diff**: which L-vars changed since the last ask (the recon tool's watch mode)

The protocol is in [docs/bridge-protocol.md](../../docs/bridge-protocol.md).

## Build

Needs the MSFS 2024 SDK (the WASM part of it).

```
cd wasm\ClikstpBridge
build.cmd
```

That compiles `src\BridgeCore.cpp` and `src\ClikstpBridge.cpp` with the SDK's clang, stages the module for the
Project Editor, and stages it in `PackageSources\modules\` for the Project Editor.
The bridge module version should be logged to the console upon load.  It is set in the `BridgeCore.h` file using the `kVersion` as the source variable name.

## Source layout

| File | What it is |
|---|---|
| `src/BridgeCore.h/.cpp` | Everything that doesn't need the sim: parsing, validation, replies, chunking, the L-var diff. Plain C++17. |
| `src/ClikstpBridge.cpp` | The thin sim layer: `SimApi` on top of the real MSFS calls, plus `module_init` / `module_deinit`. |
| `tests/FakeSim.h` | A stand-in for the MSFS API, with notes on how each real call behaves (what the bench has shown, and what's still a guess). |
| `tests/BridgeTests.cpp` | Unit tests, one section per verb of the protocol. |
| `tests/Fuzz.cpp`, `tests/corpus/` | A libFuzzer target that throws made-up packets at the command handler. |

## Tests

The core builds and runs on a desktop, under AddressSanitizer and UndefinedBehaviorSanitizer, so a buffer overrun
or use-after-free fails the test with a line number instead of crashing the sim.

- **Windows:** open a *Developer Command Prompt for VS 2022*, then `wasm\ClikstpBridge\tests\run-tests.cmd`
  (AddressSanitizer only; MSVC has no UBSan).
- **Linux, macOS or WSL:** `sh wasm/ClikstpBridge/tests/run-tests.sh` for the unit tests, `... run-tests.sh fuzz`
  to fuzz for 60 s as well (needs clang; `FUZZ_SECONDS=600` for longer). A crash leaves a `crash-*` file with the
  input that caused it.
- **In the sim:** ClikstpRecon's `selftest` command runs the protocol against the live module, then sends
  deliberately bad commands and checks each gets a clean error. It uses `L:CLIKSTP_BRIDGE_TEST`, a scratch L-var
  the module registers, and resets it to 0. It also reports how `fsVarsGetBVarId` answers a made-up name.

**To learn the API with them:** pick a verb, read its section in `BridgeTests.cpp` and the matching call in
`FakeSim.h`, then try the same thing live with Recon (`get`, `set`, `bvar`, `calc`). When the sim behaves
differently from the fake, change the fake first (with a note of what you saw), then the test, then the code.

Run the desktop tests before every Project Editor build; anything that only fails in the sim is in
`ClikstpBridge.cpp`, which is short on purpose.


## Package (MSFS 2024)

Build the package with the sim's own package builder. A hand-made `manifest.json` + `layout.json` gets registered as
an **MSFS 2020** package and its module stays disabled.

1. `build.cmd` compiles and stages `PackageSources\modules\ClikstpBridge.wasm`.
2. In the sim: Dev Mode -> **Tools -> Project Editor** -> **File -> Open** -> `ClikstpBridge.xml`.
3. **Build All**. The sim builds `Packages\clikstp-bridge` and loads it right away (no restart, no Community copy
   needed while you're in Dev Mode).
4. For everyday flying without Dev Mode: copy `Packages\clikstp-bridge` into Community (delete any older copy).

The `manifest.json` file must contain the `minimum_game_version`, `minimum_compatibility_version`, the `export_type`, and a valid `package_order_hint`.  If the package order hint ends up missing, utilize `CUSTOM_INSTRUMENT_PATCH`.

Dev Mode -> **Debug -> WASM** (WASM Debug window) lists the modules that actually loaded.

## Check that it loaded

1. Restart the sim with the package in Community.
2. Dev mode -> Console: look for `[ClikstpBridge] ready`.
3. CLIKSTP's log shows `WASM bridge found (clikstp-bridge 6)` and, once an aircraft is loaded,
   `WASM bridge: N L-vars.`
4. Without the module, the log says `WASM bridge not found` once, plus a couple of harmless SimConnect exceptions
   from using areas that don't exist. Everything else works as before.

## First experiments

Run `ClikstpRecon` with the Comanche loaded, wait for the capture, then type:

```
bvar AS5_a2amod_1_Knob_Button_Push
bvar AS5_a2amod_1_Knob_Button_Release
```

If the top G5 reacts, the knob push is solved and becomes `bvar:` actions in the G5 tools. If `bvar` says there's no
such event, try the calculator route: `calc (>B:AS5_a2amod_1_Knob_Button_Push)`. Also worth trying:
`hvar AS530_CLR_Push` (or whatever the 530's CLR H-event is called; the behaviors debugger shows it).

## Safety notes

- A crash in a WASM module can take the sim down, so the module is small, copies every incoming message into its
  own terminated buffer, and validates every number.
- It runs any calculator code a local SimConnect client sends. That's fine for a local tool, but worth knowing.
- Errors from fire-and-forget commands (encoder clicks) go to the dev mode console as `[ClikstpBridge] error: ...`.
