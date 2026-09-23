#include "ppp/level.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <set>
#include <sstream>

#include "ppp/json.h"
#include "ppp/player.h"

namespace ppp {
namespace {

AABB ReadRect(const Json& j) {
    // Two spellings, because a level file is hand-edited: {x,y,w,h} for
    // authoring and [x, y, w, h] for anything generated.
    if (j.IsArray() && j.Size() >= 4) {
        return AABB::FromRect(j[0].AsFloat(), j[1].AsFloat(), j[2].AsFloat(), j[3].AsFloat());
    }
    return AABB::FromRect(j["x"].AsFloat(), j["y"].AsFloat(), j["w"].AsFloat(), j["h"].AsFloat());
}

Vec2 ReadVec(const Json& j) {
    if (j.IsArray() && j.Size() >= 2) return {j[0].AsFloat(), j[1].AsFloat()};
    return {j["x"].AsFloat(), j["y"].AsFloat()};
}

std::string RectJson(const AABB& r) {
    std::ostringstream out;
    out << "[" << JsonNumber(r.min.x) << ", " << JsonNumber(r.min.y) << ", "
        << JsonNumber(r.Width()) << ", " << JsonNumber(r.Height()) << "]";
    return out.str();
}

std::string VecJson(const Vec2& v) {
    std::ostringstream out;
    out << "[" << JsonNumber(v.x) << ", " << JsonNumber(v.y) << "]";
    return out.str();
}

std::string Where(const char* what, size_t index) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s[%zu]", what, index);
    return buf;
}

}  // namespace

int LevelBody::ParticleCount() const {
    switch (type) {
        case BodyType::Pressure: return count;
        case BodyType::Lattice: return width * height;
        case BodyType::Rope: return count;
    }
    return 0;
}

int Level::TotalParticles() const {
    int total = 0;
    for (const auto& b : bodies) total += b.ParticleCount();
    return total;
}

const LevelDoor* Level::FindDoor(int door_id) const {
    for (const auto& d : doors)
        if (d.id == door_id) return &d;
    return nullptr;
}

const LevelPlate* Level::FindPlate(int plate_id) const {
    for (const auto& p : plates)
        if (p.id == plate_id) return &p;
    return nullptr;
}

// ------------------------------------------------------------------ load

bool LoadLevel(const std::string& text, Level* out, std::string* error) {
    Json root;
    if (!Json::Parse(text, &root, error)) return false;
    if (!root.IsObject()) {
        if (error) *error = "a level must be a JSON object";
        return false;
    }

    Level level;
    level.id = root["id"].AsString("untitled");
    level.name = root["name"].AsString(level.id);
    level.format_version = root["format_version"].AsInt(1);
    level.seed = static_cast<uint32_t>(root["seed"].AsInt(0));
    level.spawn = ReadVec(root["spawn"]);
    level.goal = ReadRect(root["goal"]);
    level.particle_budget = root["particle_budget"].AsInt(1200);
    level.min_wall_thickness = root["min_wall_thickness"].AsFloat(8.0f);

    for (const auto& b : root["boxes"].Elements()) {
        LevelBox box;
        box.rect = ReadRect(b);
        box.door_id = b.IsArray() ? -1 : b["door"].AsInt(-1);
        level.boxes.push_back(box);
    }

    for (size_t i = 0; i < root["bodies"].Size(); ++i) {
        const Json& b = root["bodies"][i];
        LevelBody body;
        const std::string type = b["type"].AsString("pressure");
        if (type == "pressure") body.type = BodyType::Pressure;
        else if (type == "lattice") body.type = BodyType::Lattice;
        else if (type == "rope") body.type = BodyType::Rope;
        else {
            if (error) *error = Where("bodies", i) + ": unknown body type '" + type + "'";
            return false;
        }
        body.id = b["id"].AsInt(static_cast<int>(i));
        body.position = ReadVec(b["position"]);
        body.radius = b["radius"].AsFloat(24.0f);
        body.count = b["count"].AsInt(20);
        body.pressure = b["pressure"].AsFloat(1.0f);
        body.width = b["width"].AsInt(4);
        body.height = b["height"].AsInt(4);
        body.spacing = b["spacing"].AsFloat(12.0f);
        body.to = ReadVec(b["to"]);
        body.pin_start = b["pin_start"].AsBool(true);
        body.pin_end = b["pin_end"].AsBool(false);
        body.mass_per_particle = b["mass"].AsFloat(1.0f);
        level.bodies.push_back(body);
    }

    for (size_t i = 0; i < root["plates"].Size(); ++i) {
        const Json& p = root["plates"][i];
        LevelPlate plate;
        plate.id = p["id"].AsInt(static_cast<int>(i));
        plate.region = ReadRect(p["region"]);
        plate.threshold = p["threshold"].AsFloat(3.0f);
        plate.player_counts = p["player_counts"].AsBool(true);
        for (const auto& d : p["opens"].Elements()) plate.opens.push_back(d.AsInt());
        level.plates.push_back(plate);
    }

    for (size_t i = 0; i < root["doors"].Size(); ++i) {
        const Json& d = root["doors"][i];
        LevelDoor door;
        door.id = d["id"].AsInt(static_cast<int>(i));
        door.latching = d["latching"].AsBool(true);
        for (const auto& p : d["plates"].Elements()) door.plates.push_back(p.AsInt());
        level.doors.push_back(door);
    }

    // A plate may declare the doors it opens, or a door may declare the
    // plates that open it. Authors reach for whichever is nearer, so both
    // are accepted and cross-linked here — once, at load, so nothing
    // downstream has to care which spelling the file used.
    for (const auto& plate : level.plates) {
        for (const int door_id : plate.opens) {
            for (auto& door : level.doors) {
                if (door.id != door_id) continue;
                if (std::find(door.plates.begin(), door.plates.end(), plate.id) ==
                    door.plates.end()) {
                    door.plates.push_back(plate.id);
                }
            }
        }
    }

    for (const auto& h : root["hazards"].Elements()) level.hazards.push_back(ReadRect(h));

    for (const auto& c : root["checkpoints"].Elements()) {
        LevelCheckpoint cp;
        if (c.IsArray()) {
            cp.position = ReadVec(c);
        } else {
            cp.position = ReadVec(c["position"]);
            cp.radius = c["radius"].AsFloat(20.0f);
        }
        level.checkpoints.push_back(cp);
    }

    if (root.Has("camera_bounds")) {
        level.camera_bounds = ReadRect(root["camera_bounds"]);
    } else {
        // Derived from the geometry, expanded a little. A level with no
        // authored bounds should still validate rather than failing the
        // "fits the camera" check against a zero-sized box.
        AABB b{{1e30f, 1e30f}, {-1e30f, -1e30f}};
        for (const auto& box : level.boxes) {
            b.min.x = Min(b.min.x, box.rect.min.x);
            b.min.y = Min(b.min.y, box.rect.min.y);
            b.max.x = Max(b.max.x, box.rect.max.x);
            b.max.y = Max(b.max.y, box.rect.max.y);
        }
        if (b.min.x > b.max.x) b = AABB::FromRect(0.0f, 0.0f, 640.0f, 360.0f);
        level.camera_bounds = b.Expanded(32.0f);
    }

    *out = std::move(level);
    return true;
}

bool LoadLevelFile(const std::string& path, Level* out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    if (!LoadLevel(text, out, error)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    return true;
}

// ----------------------------------------------------------------- write

std::string WriteLevel(const Level& level) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"id\": " << JsonEscape(level.id) << ",\n";
    o << "  \"name\": " << JsonEscape(level.name) << ",\n";
    o << "  \"format_version\": " << level.format_version << ",\n";
    o << "  \"seed\": " << level.seed << ",\n";
    o << "  \"spawn\": " << VecJson(level.spawn) << ",\n";
    o << "  \"goal\": " << RectJson(level.goal) << ",\n";
    o << "  \"camera_bounds\": " << RectJson(level.camera_bounds) << ",\n";
    o << "  \"particle_budget\": " << level.particle_budget << ",\n";
    o << "  \"min_wall_thickness\": " << JsonNumber(level.min_wall_thickness) << ",\n";

    o << "  \"boxes\": [\n";
    for (size_t i = 0; i < level.boxes.size(); ++i) {
        const LevelBox& b = level.boxes[i];
        o << "    {\"x\": " << JsonNumber(b.rect.min.x) << ", \"y\": " << JsonNumber(b.rect.min.y)
          << ", \"w\": " << JsonNumber(b.rect.Width()) << ", \"h\": " << JsonNumber(b.rect.Height());
        if (b.door_id >= 0) o << ", \"door\": " << b.door_id;
        o << "}" << (i + 1 < level.boxes.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"bodies\": [\n";
    for (size_t i = 0; i < level.bodies.size(); ++i) {
        const LevelBody& b = level.bodies[i];
        o << "    {\"id\": " << b.id << ", \"type\": ";
        switch (b.type) {
            case BodyType::Pressure:
                o << "\"pressure\", \"position\": " << VecJson(b.position)
                  << ", \"radius\": " << JsonNumber(b.radius) << ", \"count\": " << b.count
                  << ", \"pressure\": " << JsonNumber(b.pressure);
                break;
            case BodyType::Lattice:
                o << "\"lattice\", \"position\": " << VecJson(b.position)
                  << ", \"width\": " << b.width << ", \"height\": " << b.height
                  << ", \"spacing\": " << JsonNumber(b.spacing);
                break;
            case BodyType::Rope:
                o << "\"rope\", \"position\": " << VecJson(b.position)
                  << ", \"to\": " << VecJson(b.to) << ", \"count\": " << b.count
                  << ", \"pin_start\": " << (b.pin_start ? "true" : "false")
                  << ", \"pin_end\": " << (b.pin_end ? "true" : "false");
                break;
        }
        o << ", \"mass\": " << JsonNumber(b.mass_per_particle) << "}"
          << (i + 1 < level.bodies.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"plates\": [\n";
    for (size_t i = 0; i < level.plates.size(); ++i) {
        const LevelPlate& p = level.plates[i];
        o << "    {\"id\": " << p.id << ", \"region\": " << RectJson(p.region)
          << ", \"threshold\": " << JsonNumber(p.threshold)
          << ", \"player_counts\": " << (p.player_counts ? "true" : "false") << ", \"opens\": [";
        for (size_t k = 0; k < p.opens.size(); ++k) o << (k ? ", " : "") << p.opens[k];
        o << "]}" << (i + 1 < level.plates.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"doors\": [\n";
    for (size_t i = 0; i < level.doors.size(); ++i) {
        const LevelDoor& d = level.doors[i];
        o << "    {\"id\": " << d.id << ", \"latching\": " << (d.latching ? "true" : "false")
          << ", \"plates\": [";
        for (size_t k = 0; k < d.plates.size(); ++k) o << (k ? ", " : "") << d.plates[k];
        o << "]}" << (i + 1 < level.doors.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"hazards\": [";
    for (size_t i = 0; i < level.hazards.size(); ++i)
        o << (i ? ", " : "") << RectJson(level.hazards[i]);
    o << "],\n";

    o << "  \"checkpoints\": [";
    for (size_t i = 0; i < level.checkpoints.size(); ++i) {
        o << (i ? ", " : "") << "{\"position\": " << VecJson(level.checkpoints[i].position)
          << ", \"radius\": " << JsonNumber(level.checkpoints[i].radius) << "}";
    }
    o << "]\n";

    o << "}\n";
    return o.str();
}

// ------------------------------------------------------------- flood fill

bool FloodReachable(const Level& level, const Vec2& from, const AABB& to, float cell,
                    float agent_radius) {
    if (cell <= 0.0f) return false;

    // The camera bounds exactly, with no margin. An earlier version expanded
    // them by a couple of cells "for safety" and quietly broke every
    // reachability check in the suite: the margin ran underneath the floor,
    // which is the one place a level has no walls, so the fill went down one
    // side of any barrier, along the strip below the world, and up the
    // other. A sealed level reported its goal as reachable.
    //
    // Anything outside the camera bounds is not part of the level, so
    // treating the bounds as solid is both correct and what makes the check
    // mean something.
    const AABB bounds = level.camera_bounds;
    const int w = static_cast<int>(bounds.Width() / cell) + 1;
    const int h = static_cast<int>(bounds.Height() / cell) + 1;
    if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > 4'000'000LL) return false;

    // Doors count as open. A level whose goal is only reachable once a door
    // opens should still pass reachability — whether the door *can* be
    // opened is a question only the recorded solution replay answers
    // (doc 3.3), and failing it here would reject every real puzzle.
    std::vector<AABB> solid;
    solid.reserve(level.boxes.size());
    for (const auto& b : level.boxes) {
        if (b.door_id >= 0) continue;
        solid.push_back(b.rect.Expanded(agent_radius));
    }

    auto blocked = [&](int cx, int cy) {
        const Vec2 p{bounds.min.x + (static_cast<float>(cx) + 0.5f) * cell,
                     bounds.min.y + (static_cast<float>(cy) + 0.5f) * cell};
        for (const auto& s : solid)
            if (s.Contains(p)) return true;
        return false;
    };

    auto index = [&](int cx, int cy) { return static_cast<size_t>(cy) * static_cast<size_t>(w) + static_cast<size_t>(cx); };

    std::vector<uint8_t> seen(static_cast<size_t>(w) * static_cast<size_t>(h), 0);

    int sx = static_cast<int>((from.x - bounds.min.x) / cell);
    int sy = static_cast<int>((from.y - bounds.min.y) / cell);
    sx = static_cast<int>(Clamp(static_cast<float>(sx), 0.0f, static_cast<float>(w - 1)));
    sy = static_cast<int>(Clamp(static_cast<float>(sy), 0.0f, static_cast<float>(h - 1)));

    // A spawn standing on the floor sits with its feet inside the expanded
    // geometry. Nudge the seed to the nearest free cell rather than
    // reporting "unreachable" for every level with a floor.
    if (blocked(sx, sy)) {
        bool found = false;
        for (int r = 1; r <= 6 && !found; ++r) {
            for (int dy = -r; dy <= r && !found; ++dy) {
                for (int dx = -r; dx <= r && !found; ++dx) {
                    const int nx = sx + dx, ny = sy + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    if (blocked(nx, ny)) continue;
                    sx = nx;
                    sy = ny;
                    found = true;
                }
            }
        }
        if (!found) return false;
    }

    std::deque<std::pair<int, int>> queue;
    queue.push_back({sx, sy});
    seen[index(sx, sy)] = 1;

    const AABB target = to.Expanded(cell);

    while (!queue.empty()) {
        const auto [cx, cy] = queue.front();
        queue.pop_front();
        const Vec2 p{bounds.min.x + (static_cast<float>(cx) + 0.5f) * cell,
                     bounds.min.y + (static_cast<float>(cy) + 0.5f) * cell};
        if (target.Contains(p)) return true;

        static const int kDx[4] = {1, -1, 0, 0};
        static const int kDy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kDx[d];
            const int ny = cy + kDy[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if (seen[index(nx, ny)]) continue;
            if (blocked(nx, ny)) continue;
            seen[index(nx, ny)] = 1;
            queue.push_back({nx, ny});
        }
    }
    return false;
}

// -------------------------------------------------------------- validation

int ValidationReport::ErrorCount() const {
    int n = 0;
    for (const auto& i : issues)
        if (i.severity == ValidationIssue::Severity::Error) ++n;
    return n;
}

int ValidationReport::WarningCount() const {
    return static_cast<int>(issues.size()) - ErrorCount();
}

namespace {

void Error(ValidationReport* r, const char* check, const std::string& message) {
    r->issues.push_back({ValidationIssue::Severity::Error, check, message});
}

void Warn(ValidationReport* r, const char* check, const std::string& message) {
    r->issues.push_back({ValidationIssue::Severity::Warning, check, message});
}

std::string Num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return buf;
}

std::string Point(const Vec2& v) { return "(" + Num(v.x) + ", " + Num(v.y) + ")"; }

}  // namespace

ValidationReport ValidateLevel(const Level& level, const PhysicsContract& contract) {
    ValidationReport report;

    // --- wall thickness (doc 4.5) --------------------------------------
    //
    // The rule exists because a particle sweeps from `prev` to `pos` and a
    // wall thinner than that sweep can be crossed without the sweep ever
    // reporting a hit. The solver's own guard is the swept raycast; this is
    // the authoring rule that keeps levels inside what the guard covers.
    for (size_t i = 0; i < level.boxes.size(); ++i) {
        const float thickness = level.boxes[i].rect.Thickness();
        if (thickness < level.min_wall_thickness) {
            Error(&report, "wall-thickness",
                  Where("boxes", i) + " is " + Num(thickness) + " units thick, minimum is " +
                      Num(level.min_wall_thickness));
        }
    }

    // --- spawn ----------------------------------------------------------
    // The real player box, not an approximation of it: a spawn check that
    // uses a different size from the player is a check that passes for a
    // level the player then starts inside.
    const PlayerTuning& tuning = DefaultPlayerTuning();
    const AABB spawn_box{{level.spawn.x - tuning.half_width, level.spawn.y - tuning.half_height},
                         {level.spawn.x + tuning.half_width, level.spawn.y + tuning.half_height}};
    for (size_t i = 0; i < level.boxes.size(); ++i) {
        if (level.boxes[i].door_id >= 0) continue;
        if (spawn_box.Overlaps(level.boxes[i].rect)) {
            Error(&report, "spawn-inside-geometry",
                  "the spawn at " + Point(level.spawn) + " is inside " + Where("boxes", i));
            break;
        }
    }
    for (size_t i = 0; i < level.hazards.size(); ++i) {
        if (spawn_box.Overlaps(level.hazards[i])) {
            Error(&report, "spawn-inside-hazard",
                  "the spawn at " + Point(level.spawn) + " is inside " + Where("hazards", i));
            break;
        }
    }

    // --- soft bodies not overlapping geometry at rest (doc 9.3) ---------
    //
    // A body that starts inside a wall is pushed out on the first step,
    // which at these stiffnesses looks like an explosion.
    for (size_t i = 0; i < level.bodies.size(); ++i) {
        const LevelBody& b = level.bodies[i];
        AABB extent;
        switch (b.type) {
            case BodyType::Pressure:
                extent = AABB{{b.position.x - b.radius, b.position.y - b.radius},
                              {b.position.x + b.radius, b.position.y + b.radius}};
                break;
            case BodyType::Lattice:
                extent = AABB{b.position,
                              {b.position.x + static_cast<float>(b.width - 1) * b.spacing,
                               b.position.y + static_cast<float>(b.height - 1) * b.spacing}};
                break;
            case BodyType::Rope:
                extent = AABB{{Min(b.position.x, b.to.x), Min(b.position.y, b.to.y)},
                              {Max(b.position.x, b.to.x), Max(b.position.y, b.to.y)}};
                break;
        }
        extent = extent.Expanded(contract.particle_radius);
        for (size_t k = 0; k < level.boxes.size(); ++k) {
            if (level.boxes[k].door_id >= 0) continue;
            if (!extent.Overlaps(level.boxes[k].rect)) continue;
            // A rope is *meant* to be pinned to geometry, so its endpoints
            // overlapping a wall is the normal case, not an error.
            if (b.type == BodyType::Rope && (b.pin_start || b.pin_end)) {
                Warn(&report, "body-overlaps-geometry",
                     Where("bodies", i) + " (rope) overlaps " + Where("boxes", k) +
                         "; expected if it is pinned there");
            } else {
                Error(&report, "body-overlaps-geometry",
                      Where("bodies", i) + " starts inside " + Where("boxes", k));
            }
            break;
        }
    }

    // --- particle budget (doc 10.1) -------------------------------------
    const int particles = level.TotalParticles();
    if (particles > level.particle_budget) {
        Error(&report, "particle-budget",
              std::to_string(particles) + " particles, budget is " +
                  std::to_string(level.particle_budget));
    } else if (particles > level.particle_budget * 9 / 10) {
        Warn(&report, "particle-budget",
             std::to_string(particles) + " particles is within 10% of the budget of " +
                 std::to_string(level.particle_budget));
    }

    // --- camera bounds ---------------------------------------------------
    auto inside_camera = [&](const AABB& r) {
        return r.min.x >= level.camera_bounds.min.x && r.max.x <= level.camera_bounds.max.x &&
               r.min.y >= level.camera_bounds.min.y && r.max.y <= level.camera_bounds.max.y;
    };
    for (size_t i = 0; i < level.boxes.size(); ++i) {
        if (!inside_camera(level.boxes[i].rect)) {
            Warn(&report, "camera-bounds", Where("boxes", i) + " extends outside the camera bounds");
        }
    }
    if (!level.camera_bounds.Contains(level.spawn)) {
        Error(&report, "camera-bounds", "the spawn is outside the camera bounds");
    }
    if (!inside_camera(level.goal)) {
        Error(&report, "camera-bounds", "the goal is outside the camera bounds");
    }

    // --- reachability (doc 9.3) -------------------------------------------
    const float cell = 6.0f;
    if (!FloodReachable(level, level.spawn, level.goal, cell, 2.0f)) {
        Error(&report, "goal-unreachable",
              "a flood fill from the spawn never reaches the goal; the exit is walled off");
    }

    // --- checkpoints -------------------------------------------------------
    if (level.checkpoints.empty()) {
        Warn(&report, "checkpoints",
             "no checkpoints; the level restarts from the spawn, which is fine only if it is short");
    }
    for (size_t i = 0; i < level.checkpoints.size(); ++i) {
        const AABB cp{{level.checkpoints[i].position.x - 1.0f, level.checkpoints[i].position.y - 1.0f},
                      {level.checkpoints[i].position.x + 1.0f, level.checkpoints[i].position.y + 1.0f}};
        if (!FloodReachable(level, level.spawn, cp, cell, 2.0f)) {
            Error(&report, "checkpoint-unreachable",
                  Where("checkpoints", i) + " at " + Point(level.checkpoints[i].position) +
                      " is not reachable from the spawn");
        }
    }

    // --- ids and wiring -----------------------------------------------------
    std::set<int> body_ids, plate_ids, door_ids;
    for (size_t i = 0; i < level.bodies.size(); ++i) {
        if (!body_ids.insert(level.bodies[i].id).second) {
            Error(&report, "duplicate-id",
                  "two bodies share id " + std::to_string(level.bodies[i].id) +
                      "; replays address bodies by id (doc 6.2)");
        }
    }
    for (const auto& p : level.plates) {
        if (!plate_ids.insert(p.id).second)
            Error(&report, "duplicate-id", "two plates share id " + std::to_string(p.id));
    }
    for (const auto& d : level.doors) {
        if (!door_ids.insert(d.id).second)
            Error(&report, "duplicate-id", "two doors share id " + std::to_string(d.id));
    }
    for (const auto& p : level.plates) {
        for (const int door_id : p.opens) {
            if (door_ids.count(door_id) == 0) {
                Error(&report, "dangling-reference",
                      "plate " + std::to_string(p.id) + " opens door " + std::to_string(door_id) +
                          ", which does not exist");
            }
        }
        if (p.opens.empty()) {
            Warn(&report, "unused-plate", "plate " + std::to_string(p.id) + " opens nothing");
        }
    }
    for (const auto& d : level.doors) {
        if (d.plates.empty()) {
            Error(&report, "unopenable-door",
                  "door " + std::to_string(d.id) + " has no plates and can never open");
        }
        bool has_box = false;
        for (const auto& b : level.boxes)
            if (b.door_id == d.id) has_box = true;
        if (!has_box) {
            Warn(&report, "door-has-no-geometry",
                 "door " + std::to_string(d.id) + " has no boxes, so opening it does nothing");
        }
    }
    for (const auto& b : level.boxes) {
        if (b.door_id >= 0 && door_ids.count(b.door_id) == 0) {
            Error(&report, "dangling-reference",
                  "a box belongs to door " + std::to_string(b.door_id) + ", which does not exist");
        }
    }

    if (level.goal.Width() <= 0.0f || level.goal.Height() <= 0.0f) {
        Error(&report, "goal", "the goal has no area");
    }

    return report;
}

}  // namespace ppp
