// BridgeCore: see BridgeCore.h. No MSFS headers here, so this file builds and runs anywhere.

#include "BridgeCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace clikstp
{
    // --- Helpers ---------------------------------------------------------------------------------

    std::string Number(double v)
    {
        char text[64];
        snprintf(text, sizeof text, "%.10g", v);
        return text;
    }

    bool ParseNumber(const std::string& text, double& out)
    {
        if (text.empty()) return false;
        char* end = nullptr;
        out = strtod(text.c_str(), &end);
        return end && *end == '\0' && std::isfinite(out);
    }

    // Accept "NAME" or "L:NAME"; strip a ", unit" suffix if someone pastes one.
    std::string LVarName(std::string name)
    {
        if (name.size() > 2 && (name[0] == 'L' || name[0] == 'l') && name[1] == ':') name = name.substr(2);
        size_t comma = name.find(',');
        if (comma != std::string::npos) name = name.substr(0, comma);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        return name;
    }

    std::string Unprefixed(std::string name, char letter)
    {
        if (name.size() > 2 && (name[0] == letter || name[0] == letter + ('a' - 'A')) && name[1] == ':') name = name.substr(2);
        return name;
    }

    // Up to maxParts space-separated words; the last part keeps the rest of the line, spaces and all (calc code).
    std::vector<std::string> Split(const std::string& text, size_t maxParts)
    {
        std::vector<std::string> parts;
        if (maxParts == 0) return parts;
        size_t i = 0;
        while (i < text.size() && parts.size() + 1 < maxParts)
        {
            while (i < text.size() && text[i] == ' ') ++i;
            if (i >= text.size()) break;
            size_t j = text.find(' ', i);
            if (j == std::string::npos) j = text.size();
            parts.push_back(text.substr(i, j - i));
            i = j;
        }
        while (i < text.size() && text[i] == ' ') ++i;
        if (i < text.size()) parts.push_back(text.substr(i));
        return parts;
    }

    // --- Replies ---------------------------------------------------------------------------------

    void Bridge::SendText(const std::string& text)
    {
        static char buffer[kResponseSize];
        memset(buffer, 0, sizeof buffer);
        size_t n = text.size() < kResponseSize - 1 ? text.size() : kResponseSize - 1;
        memcpy(buffer, text.data(), n);
        sim_.Send(buffer, kResponseSize);
    }

    void Bridge::Reply(const std::string& id, const std::string& status, const std::string& payload)
    {
        if (id == "-")
        {
            if (status == "err") sim_.Log("error: " + payload);
            return;
        }
        SendText(payload.empty() ? id + " " + status : id + " " + status + " " + payload);
    }

    // Long lists go out as numbered "part" messages, then a final "ok <count>". Each part is "<id> part <seq>\n"
    // followed by lines; lines never split across parts. A line too long for any part is cut to fit (and counted),
    // so one bad L-var name can't make the list overflow or vanish.
    class Chunker
    {
    public:
        Chunker(Bridge& bridge, std::string id) : bridge_(bridge), id_(std::move(id)) { Start(); }

        void Line(std::string line)
        {
            size_t room = kResponseSize - 2 - header_.size();   // NUL and the line's '\n'
            if (line.size() > room) line.resize(room);
            if (header_.size() + body_.size() + line.size() + 1 > kResponseSize - 1) Flush();
            body_ += line;
            body_ += '\n';
            ++count_;
        }

        void Finish()
        {
            if (!body_.empty()) Flush();
            bridge_.Reply(id_, "ok", std::to_string(count_));
        }

    private:
        void Start() { header_ = id_ + " part " + std::to_string(seq_) + "\n"; }
        void Flush()
        {
            if (id_ != "-") bridge_.SendText(header_ + body_);
            body_.clear();
            ++seq_;
            Start();
        }

        Bridge& bridge_;
        std::string id_, header_, body_;
        int seq_ = 0, count_ = 0;
    };

    // --- L-var walk ------------------------------------------------------------------------------

    template <typename F> void Bridge::ForEachLVar(F f)
    {
        int misses = 0;
        std::string name;
        for (int id = 0; misses < kMaxIdGap && id < kMaxLVarId; ++id)
        {
            if (!sim_.LVarName(id, name)) { ++misses; continue; }
            misses = 0;
            double value = 0;
            if (!sim_.GetLVar(id, value)) value = NAN;
            f(id, name, value);
        }
    }

    // --- Commands --------------------------------------------------------------------------------

    void Bridge::Calc(const std::string& id, const std::string& code)
    {
        double result = 0;
        if (!sim_.Calc(code, result)) { Reply(id, "err", "calculator code failed: " + code); return; }
        Reply(id, "ok", Number(result));
    }

    void Bridge::ListLVars(const std::string& id)
    {
        Chunker out(*this, id);
        ForEachLVar([&](int, const std::string& name, double value) { out.Line(name + "\t" + Number(value)); });
        out.Finish();
    }

    // Changed L-vars since the previous diff. An L-var seen for the first time is only baselined, so the first diff
    // after load (or after Reset) reports nothing.
    void Bridge::Diff(const std::string& id)
    {
        Chunker out(*this, id);
        ForEachLVar([&](int i, const std::string& name, double now)
        {
            size_t at = (size_t)i;
            if (at >= snapshot_.size()) snapshot_.resize(at + 1, NAN);
            double was = snapshot_[at];
            snapshot_[at] = now;
            if (std::isnan(was)) return;                 // first sight, or unreadable last time
            if (was != now) out.Line(name + "\t" + Number(was) + "\t" + Number(now));
        });
        out.Finish();
    }

    bool Bridge::FindLVar(const std::string& id, const std::string& raw, int& out)
    {
        std::string name = LVarName(raw);
        out = name.empty() ? kInvalidId : sim_.FindLVar(name);
        if (out == kInvalidId) { Reply(id, "err", "no L-var named " + name); return false; }
        return true;
    }

    void Bridge::Get(const std::string& id, const std::vector<std::string>& a)
    {
        int v;
        double value = 0;
        if (a.size() < 1) { Reply(id, "err", "usage: get NAME"); return; }
        if (!FindLVar(id, a[0], v)) return;
        if (!sim_.GetLVar(v, value)) { Reply(id, "err", "could not read " + a[0]); return; }
        Reply(id, "ok", Number(value));
    }

    // getmany NAME NAME ...: several L-vars in one round trip (the app's once-a-second poll of the L-vars it watches).
    // A list reply, one "name<TAB>value" line per name in the order asked; "-" for a name with no L-var (or an
    // unreadable one), so the client can tell "missing" from 0.
    void Bridge::GetMany(const std::string& id, const std::string& names)
    {
        if (names.empty()) { Reply(id, "err", "usage: getmany NAME [NAME...]"); return; }
        Chunker out(*this, id);
        for (const auto& raw : Split(names, kCommandSize))
        {
            std::string name = LVarName(raw);
            int v = name.empty() ? kInvalidId : sim_.FindLVar(name);
            double value = 0;
            if (v == kInvalidId || !sim_.GetLVar(v, value) || !std::isfinite(value)) out.Line(name + "\t-");
            else out.Line(name + "\t" + Number(value));
        }
        out.Finish();
    }

    void Bridge::Set(const std::string& id, const std::vector<std::string>& a)
    {
        int v;
        double value;
        if (a.size() < 2 || !ParseNumber(a[1], value)) { Reply(id, "err", "usage: set NAME VALUE"); return; }
        if (!FindLVar(id, a[0], v)) return;
        if (!sim_.SetLVar(v, value)) { Reply(id, "err", "could not write " + a[0]); return; }
        Reply(id, "ok", Number(value));
    }

    // step NAME AMOUNT [MIN MAX]: read, add, clamp, write, all in one frame. Default range 0..1 (dimmers).
    void Bridge::Step(const std::string& id, const std::vector<std::string>& a)
    {
        int v;
        double amount = 0, lo = 0, hi = 1, value = 0;
        bool ok = a.size() >= 2 && ParseNumber(a[1], amount);
        if (ok && a.size() >= 3) ok = a.size() >= 4 && ParseNumber(a[2], lo) && ParseNumber(a[3], hi) && lo < hi;
        if (!ok) { Reply(id, "err", "usage: step NAME AMOUNT [MIN MAX]"); return; }
        if (!FindLVar(id, a[0], v)) return;

        if (!sim_.GetLVar(v, value)) { Reply(id, "err", "could not read " + a[0]); return; }
        if (!std::isfinite(value)) value = lo;
        value += amount;
        if (value < lo) value = lo;
        if (value > hi) value = hi;
        value = std::round(value * 10000.0) / 10000.0;
        if (!sim_.SetLVar(v, value)) { Reply(id, "err", "could not write " + a[0]); return; }
        Reply(id, "ok", Number(value));
    }

    // hvar NAME: fire an H-event. Through calculator code: fsEventsHEventCall returned without the cockpit reacting
    // on the bench (A2A Comanche GTN750_HomePush), while "(>H:NAME)" worked.
    void Bridge::HVar(const std::string& id, const std::vector<std::string>& a)
    {
        if (a.size() < 1) { Reply(id, "err", "usage: hvar NAME"); return; }
        std::string name = Unprefixed(a[0], 'H');
        if (name.find_first_of("() ") != std::string::npos) { Reply(id, "err", "bad H-event name " + name); return; }
        Calc(id, "(>H:" + name + ")");
    }

    // bvar NAME: call a B: event on the user aircraft, e.g. AS5_a2amod_1_Knob_Button_Push. Looked up on every call:
    // B: ids belong to the loaded aircraft, which can change.
    void Bridge::BVar(const std::string& id, const std::vector<std::string>& a)
    {
        if (a.size() < 1) { Reply(id, "err", "usage: bvar NAME"); return; }
        std::string name = Unprefixed(a[0], 'B');
        int b = sim_.FindBVar(name);
        if (b == kInvalidId) { Reply(id, "err", "no callable B: event named " + name); return; }
        unsigned e = sim_.CallBVar(b);
        if (e != 0) { Reply(id, "err", "B: event " + name + " failed (" + std::to_string(e) + ")"); return; }
        Reply(id, "ok");
    }

    // --- Entry points ----------------------------------------------------------------------------

    void Bridge::HandlePacket(const char* data, size_t size)
    {
        if (!data) return;
        size_t n = size < kCommandSize ? size : kCommandSize;
        char text[kCommandSize + 1];
        memcpy(text, data, n);
        text[n] = '\0';
        Handle(std::string(text));   // stops at the first NUL
    }

    void Bridge::Handle(const std::string& text)
    {
        std::string line = text;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) return;

        auto head = Split(line, 3);   // id, verb, rest
        if (head.size() < 2) { sim_.Log("ignored a malformed command"); return; }
        const std::string& id = head[0];
        const std::string& verb = head[1];
        std::string rest = head.size() > 2 ? head[2] : "";

        if (verb == "ping")       Reply(id, "ok", std::string("clikstp-bridge ") + kVersion);
        else if (verb == "calc")  { if (rest.empty()) Reply(id, "err", "usage: calc CODE"); else Calc(id, rest); }
        else if (verb == "lvars") ListLVars(id);
        else if (verb == "diff")  Diff(id);
        else if (verb == "get")   Get(id, Split(rest, 2));
        else if (verb == "getmany") GetMany(id, rest);
        else if (verb == "set")   Set(id, Split(rest, 3));
        else if (verb == "step")  Step(id, Split(rest, 5));
        else if (verb == "hvar")  HVar(id, Split(rest, 2));
        else if (verb == "bvar")  BVar(id, Split(rest, 2));
        else Reply(id, "err", "unknown command " + verb);
    }
}
