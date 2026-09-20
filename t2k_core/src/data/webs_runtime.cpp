#include "webs_runtime.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "levels_json.h"   // generated at build time from data/levels.json

namespace ts {
namespace {

// Storage for the loaded webs. Each WebDef points into `names`, so the two
// live exactly as long as each other; everything is sized once per load and
// never grown, so nothing reallocates under a published pointer.
struct Loaded {
    std::vector<WebDef> webs;
    std::vector<std::string> names;
    WebSet published{};
    bool ready = false;
};

Loaded& live() {
    static Loaded l;
    return l;
}

// A web has to survive the same checks tools/bin2h.py applies at build time,
// because a hand-edited on-disk file is exactly where a zero-length lane or an
// over-long web will come from -- and those are undefined behaviour in the
// claw/normal math, not merely ugly.
bool validWeb(const nlohmann::json& w, int& lanesOut) {
    auto dx = w.find("dx");
    auto dy = w.find("dy");
    if (dx == w.end() || dy == w.end() || !dx->is_array() || !dy->is_array()) return false;
    const int n = (int)dx->size();
    if (n != (int)dy->size()) return false;
    if (n < 3 || n > WEB_MAX_LANES) return false;
    for (int i = 0; i < n; ++i) {
        if (!(*dx)[i].is_number() || !(*dy)[i].is_number()) return false;
        const float a = (*dx)[i].get<float>();
        const float b = (*dy)[i].get<float>();
        if (a == 0.0f && b == 0.0f) return false;   // zero-length lane: no direction
    }
    lanesOut = n;
    return true;
}

// Parse a full document into fresh storage. Touches no live state, so a
// rejected file leaves whatever is currently published untouched.
bool parseWebs(const nlohmann::json& data, Loaded& out) {
    if (!data.is_object()) return false;
    auto webs = data.find("webs");
    if (webs == data.end() || !webs->is_array() || webs->empty()) return false;

    const int n = (int)webs->size();
    out.webs.resize(n);
    out.names.resize(n);
    for (int i = 0; i < n; ++i) {
        const nlohmann::json& jw = (*webs)[i];
        int lanes = 0;
        if (!jw.is_object() || !validWeb(jw, lanes)) {
            std::printf("[levels] web %d is invalid\n", i);
            return false;
        }
        auto nm = jw.find("name");
        out.names[i] = (nm != jw.end() && nm->is_string()) ? nm->get<std::string>() : "web";
        auto cl = jw.find("closed");
        WebDef& d = out.webs[i];
        std::memset(&d, 0, sizeof(d));
        d.lane_count = (uint8_t)lanes;          // implied by len(dx); no field to desync
        d.go_round   = (cl != jw.end() && cl->is_boolean()) ? cl->get<bool>() : false;
        const nlohmann::json& adx = *jw.find("dx");
        const nlohmann::json& ady = *jw.find("dy");
        for (int k = 0; k < lanes; ++k) {
            d.dx[k] = adx[k].get<float>();
            d.dy[k] = ady[k].get<float>();
        }
    }
    return true;
}

// Commit freshly parsed storage as the live levels: move it in, then point
// each WebDef at its (now final) name string and publish the row pointer.
void publish(Loaded&& fresh) {
    Loaded& l = live();
    l.webs = std::move(fresh.webs);
    l.names = std::move(fresh.names);
    for (size_t i = 0; i < l.webs.size(); ++i)
        l.webs[i].name = l.names[i].c_str();
    l.published.webs  = l.webs.data();
    l.published.count = (int)l.webs.size();
    l.ready = true;
}

// Last-resort web so levels() can never return an empty set even if the
// build-validated embedded copy somehow fails to parse (which bin2h.py makes
// unreachable short of memory corruption): one closed unit square.
void publishEmergency() {
    static const WebDef SQUARE = {
        "Emergency square", 4, true,
        { 1.0f, 0.0f, -1.0f,  0.0f },
        { 0.0f, 1.0f,  0.0f, -1.0f },
    };
    Loaded& l = live();
    l.published = { &SQUARE, 1 };
    l.ready = true;
}

// Boot state: parse the embedded copy the moment anyone asks for the levels,
// so there is no wrong order to call things in.
void ensureLoaded() {
    if (live().ready) return;
    nlohmann::json data = nlohmann::json::parse(LEVELS_JSON_EMBEDDED, nullptr, false);
    Loaded fresh;
    if (!data.is_discarded() && parseWebs(data, fresh)) {
        publish(std::move(fresh));
    } else {
        std::printf("[levels] EMBEDDED copy failed to parse -- emergency web\n");
        publishEmergency();
    }
}

} // namespace

const WebSet& levels() {
    ensureLoaded();
    return live().published;
}

bool loadLevelsJson(const char* path) {
    ensureLoaded();
    std::ifstream file(path);
    if (!file.is_open()) return false;

    // allow_exceptions=false to stay inside the -fno-exceptions doctrine, the
    // same contract save_load.cpp uses for the config.
    nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
    Loaded fresh;
    if (data.is_discarded() || !parseWebs(data, fresh)) {
        std::printf("[levels] %s is malformed -- keeping current levels\n", path);
        return false;
    }
    publish(std::move(fresh));
    std::printf("[levels] loaded %s (%d webs)\n", path, live().published.count);
    return true;
}

bool seedLevelsJsonIfMissing(const char* path) {
    // Seed when the file is missing -- and RESEED when the embedded data is
    // NEWER (its "version" is higher): a level-data update must reach an SD
    // card that was seeded by an older build, or the game silently plays the
    // old set forever (this shipped a stale 75-web card once). A player's
    // edits to the CURRENT version are never touched; bumping "version" in
    // tools/gen_webs.py is the explicit act that declares new data worth
    // overwriting them.
    {
        std::ifstream probe(path);
        if (probe.is_open()) {
            nlohmann::json onDisk = nlohmann::json::parse(probe, nullptr, false);
            const int fileVer = (!onDisk.is_discarded() && onDisk.is_object())
                                    ? onDisk.value("version", 0) : 0;
            nlohmann::json emb = nlohmann::json::parse(LEVELS_JSON_EMBEDDED, nullptr, false);
            const int embVer = (!emb.is_discarded() && emb.is_object())
                                   ? emb.value("version", 0) : 0;
            if (fileVer >= embVer) return false;    // current or edited: keep it
            std::printf("[levels] %s is version %d, embedded is %d -- reseeding\n",
                        path, fileVer, embVer);
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(LEVELS_JSON_EMBEDDED, LEVELS_JSON_EMBEDDED_SIZE);
    std::printf("[levels] seeded %s\n", path);
    return true;
}

} // namespace ts
