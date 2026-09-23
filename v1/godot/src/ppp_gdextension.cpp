// The Godot binding (doc 8, doc 10.2, doc 12).
//
// The solver is native because it has to be: doc 10.1 budgets ~9.2 million
// constraint solves per second, and GDScript is two orders of magnitude off
// that. This file is the only place the solver meets an engine, and it is
// deliberately thin — it moves data across the boundary and does no physics
// and no game logic of its own.
//
// The important structural point (doc 12) is the direction of the
// dependency. `libppp_game` knows nothing about Godot; this file knows about
// both. That is what lets the same simulation run headless in CI, which is
// the whole basis of the replay regression suite (doc 15.2). If a single
// gameplay decision moved into a `_process` somewhere in GDScript, the
// thing CI verifies would stop being the thing the player plays.
//
// Two nodes are exposed:
//
//   PppSim     — owns a Sim. The game drives it with `advance(delta)`, which
//                runs the fixed-step accumulator and reports how many steps
//                ran; rendering reads interpolated positions.
//   PppSoftBody — a @tool node that draws a body's rest shape in the editor
//                (doc 9.1), so authoring is not invisible until you press
//                play.

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <memory>
#include <string>

#include "ppp/level.h"
#include "ppp/replay.h"
#include "ppp/sim.h"

using namespace godot;

namespace {

inline Vector2 ToGodot(const ppp::Vec2& v) { return Vector2(v.x, v.y); }

/// Reads a file through Godot's FileAccess rather than std::ifstream, so
/// `res://` paths and packed .pck archives work. An exported game has no
/// filesystem at those paths at all.
bool ReadResource(const String& path, std::string* out) {
    Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_null()) return false;
    const PackedByteArray bytes = file->get_buffer(file->get_length());
    out->assign(reinterpret_cast<const char*>(bytes.ptr()), static_cast<size_t>(bytes.size()));
    return true;
}

}  // namespace

// ---------------------------------------------------------------- PppSim

class PppSim : public Node2D {
    GDCLASS(PppSim, Node2D)

public:
    PppSim() = default;
    ~PppSim() override = default;

    // --- loading -------------------------------------------------------

    bool load_level(const String& path) {
        std::string text;
        if (!ReadResource(path, &text)) {
            UtilityFunctions::push_error("PppSim: cannot read level ", path);
            return false;
        }
        std::string error;
        ppp::Level level;
        if (!ppp::LoadLevel(text, &level, &error)) {
            UtilityFunctions::push_error("PppSim: ", String(error.c_str()));
            return false;
        }
        level_ = level;
        sim_ = std::make_unique<ppp::Sim>(level_);
        clock_.Reset();
        return true;
    }

    void restart() {
        if (sim_) sim_->Reset();
        clock_.Reset();
        playback_tick_ = 0;
    }

    // --- stepping -------------------------------------------------------

    /// Runs whole fixed steps for `delta` seconds of wall clock and returns
    /// how many ran. The accumulator, its clamp and its step cap live in
    /// C++ (sim.h) rather than in GDScript, because doc 4.2's guards are
    /// exactly the sort of thing that gets "simplified" in a script file
    /// and then only bites on a machine that stutters.
    int advance(double delta) {
        if (!sim_) return 0;
        const int steps = clock_.Advance(static_cast<float>(delta));
        for (int i = 0; i < steps; ++i) {
            ppp::InputState in = pending_input_;
            if (playing_back_) {
                in = replay_.At(playback_tick_);
                ++playback_tick_;
            }
            const ppp::SimOutcome outcome = sim_->Step(in);
            if (recording_) recorder_->RecordTick(in);
            if (outcome == ppp::SimOutcome::Completed) {
                emit_signal("level_completed", sim_->Tick());
                break;
            }
            if (outcome == ppp::SimOutcome::Died) emit_signal("player_died", sim_->Deaths());
        }
        return steps;
    }

    /// The renderer's interpolation factor for this frame (doc 4.2). Every
    /// drawn position must go through this or motion judders whenever the
    /// display rate is not a multiple of 120 Hz.
    double alpha() const { return static_cast<double>(clock_.Alpha()); }

    // --- input -----------------------------------------------------------

    /// Called from GDScript once per frame with the already-deadzoned stick.
    /// The value is quantized inside Sim, so live play and a replay of it
    /// consume identical floats (doc 14.3).
    void set_input(double move_x, bool jump, bool grab, bool restart_pressed) {
        pending_input_.move_x = static_cast<float>(move_x);
        pending_input_.jump = jump;
        pending_input_.grab = grab;
        pending_input_.restart = restart_pressed;
    }

    // --- rendering ---------------------------------------------------------

    Vector2 player_position() const {
        if (!sim_) return Vector2();
        return ToGodot(sim_->InterpolatedPlayerPosition(clock_.Alpha()));
    }

    Vector2 player_size() const {
        if (!sim_) return Vector2();
        return Vector2(sim_->Tuning().half_width * 2.0f, sim_->Tuning().half_height * 2.0f);
    }

    bool player_on_floor() const { return sim_ && sim_->GetPlayer().on_floor; }
    int grabbed_body() const { return sim_ ? sim_->GetPlayer().grabbed_body : -1; }

    int body_count() const { return sim_ ? static_cast<int>(sim_->GetSolver().Bodies().size()) : 0; }

    /// A body's outline, for drawing. One array per body per frame is a few
    /// dozen Vector2 copies — nothing next to the solve, and far cheaper
    /// than exposing particles individually across the binding.
    PackedVector2Array body_outline(int index) const {
        PackedVector2Array out;
        if (!sim_) return out;
        const auto& bodies = sim_->GetSolver().Bodies();
        if (index < 0 || index >= static_cast<int>(bodies.size())) return out;
        const auto& body = bodies[static_cast<size_t>(index)];
        const auto& particles = sim_->GetSolver().Particles();
        const std::vector<int>& ring = body.ring.empty() ? body.particles : body.ring;
        out.resize(static_cast<int64_t>(ring.size()));
        for (size_t i = 0; i < ring.size(); ++i)
            out[static_cast<int64_t>(i)] = ToGodot(particles.pos[static_cast<size_t>(ring[i])]);
        return out;
    }

    bool body_sleeping(int index) const {
        if (!sim_) return false;
        const auto& bodies = sim_->GetSolver().Bodies();
        if (index < 0 || index >= static_cast<int>(bodies.size())) return false;
        return bodies[static_cast<size_t>(index)].sleeping;
    }

    bool door_open(int door_id) const {
        if (!sim_) return false;
        for (const auto& d : sim_->Doors())
            if (d.id == door_id) return d.open;
        return false;
    }

    bool plate_pressed(int plate_id) const {
        if (!sim_) return false;
        for (const auto& p : sim_->Plates())
            if (p.id == plate_id) return p.pressed;
        return false;
    }

    // --- state ---------------------------------------------------------------

    int tick() const { return sim_ ? sim_->Tick() : 0; }
    int deaths() const { return sim_ ? sim_->Deaths() : 0; }
    bool completed() const { return sim_ && sim_->Completed(); }
    String contract_hash() const {
        if (!sim_) return String("0");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(sim_->ContractHash()));
        return String(buf);
    }

    // --- replays ---------------------------------------------------------------

    void start_recording() {
        if (!sim_) return;
        recorder_ = std::make_unique<ppp::ReplayRecorder>(level_.id, *sim_);
        recording_ = true;
    }

    bool save_recording(const String& path) {
        if (!recorder_ || !sim_) return false;
        recorder_->Finish(*sim_);
        const std::string bytes = ppp::EncodeReplay(recorder_->Get());
        Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
        if (file.is_null()) return false;
        PackedByteArray out;
        out.resize(static_cast<int64_t>(bytes.size()));
        for (size_t i = 0; i < bytes.size(); ++i)
            out[static_cast<int64_t>(i)] = static_cast<uint8_t>(bytes[i]);
        file->store_buffer(out);
        recording_ = false;
        return true;
    }

    /// Plays a recorded solution back. Used by the in-game hint system and,
    /// more importantly, to watch a CI failure rather than read about it.
    bool play_replay(const String& path) {
        std::string bytes;
        if (!ReadResource(path, &bytes)) return false;
        std::string error;
        if (!ppp::DecodeReplay(bytes, &replay_, &error)) {
            UtilityFunctions::push_error("PppSim: ", String(error.c_str()));
            return false;
        }
        if (sim_ && replay_.contract_hash != sim_->ContractHash()) {
            // Reported, never silently trusted (doc 3.3).
            UtilityFunctions::push_warning(
                "PppSim: this replay was recorded under different physics and may "
                "no longer complete the level");
        }
        restart();
        playing_back_ = true;
        return true;
    }

    void stop_replay() { playing_back_ = false; }
    bool is_playing_replay() const { return playing_back_; }

protected:
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("load_level", "path"), &PppSim::load_level);
        ClassDB::bind_method(D_METHOD("restart"), &PppSim::restart);
        ClassDB::bind_method(D_METHOD("advance", "delta"), &PppSim::advance);
        ClassDB::bind_method(D_METHOD("alpha"), &PppSim::alpha);
        ClassDB::bind_method(D_METHOD("set_input", "move_x", "jump", "grab", "restart_pressed"),
                             &PppSim::set_input);
        ClassDB::bind_method(D_METHOD("player_position"), &PppSim::player_position);
        ClassDB::bind_method(D_METHOD("player_size"), &PppSim::player_size);
        ClassDB::bind_method(D_METHOD("player_on_floor"), &PppSim::player_on_floor);
        ClassDB::bind_method(D_METHOD("grabbed_body"), &PppSim::grabbed_body);
        ClassDB::bind_method(D_METHOD("body_count"), &PppSim::body_count);
        ClassDB::bind_method(D_METHOD("body_outline", "index"), &PppSim::body_outline);
        ClassDB::bind_method(D_METHOD("body_sleeping", "index"), &PppSim::body_sleeping);
        ClassDB::bind_method(D_METHOD("door_open", "door_id"), &PppSim::door_open);
        ClassDB::bind_method(D_METHOD("plate_pressed", "plate_id"), &PppSim::plate_pressed);
        ClassDB::bind_method(D_METHOD("tick"), &PppSim::tick);
        ClassDB::bind_method(D_METHOD("deaths"), &PppSim::deaths);
        ClassDB::bind_method(D_METHOD("completed"), &PppSim::completed);
        ClassDB::bind_method(D_METHOD("contract_hash"), &PppSim::contract_hash);
        ClassDB::bind_method(D_METHOD("start_recording"), &PppSim::start_recording);
        ClassDB::bind_method(D_METHOD("save_recording", "path"), &PppSim::save_recording);
        ClassDB::bind_method(D_METHOD("play_replay", "path"), &PppSim::play_replay);
        ClassDB::bind_method(D_METHOD("stop_replay"), &PppSim::stop_replay);
        ClassDB::bind_method(D_METHOD("is_playing_replay"), &PppSim::is_playing_replay);

        ADD_SIGNAL(MethodInfo("level_completed", PropertyInfo(Variant::INT, "tick")));
        ADD_SIGNAL(MethodInfo("player_died", PropertyInfo(Variant::INT, "deaths")));
    }

private:
    ppp::Level level_;
    std::unique_ptr<ppp::Sim> sim_;
    ppp::StepClock clock_;
    ppp::InputState pending_input_;

    std::unique_ptr<ppp::ReplayRecorder> recorder_;
    ppp::Replay replay_;
    bool recording_ = false;
    bool playing_back_ = false;
    int playback_tick_ = 0;
};

// ------------------------------------------------------------ PppSoftBody

/// An authoring node. Its only job is to draw the body it describes while
/// the editor is open (doc 9.1) — authoring an invisible object and pressing
/// play to see where it went is a slow loop, and at the level counts doc 3.1
/// projects, a slow loop is the whole schedule.
class PppSoftBody : public Node2D {
    GDCLASS(PppSoftBody, Node2D)

public:
    void set_body_type(int v) { body_type_ = v; queue_redraw(); }
    int get_body_type() const { return body_type_; }
    void set_particle_count(int v) { particle_count_ = v < 3 ? 3 : v; queue_redraw(); }
    int get_particle_count() const { return particle_count_; }
    void set_radius(double v) { radius_ = v; queue_redraw(); }
    double get_radius() const { return radius_; }
    void set_pressure(double v) { pressure_ = v; queue_redraw(); }
    double get_pressure() const { return pressure_; }
    void set_grid(const Vector2i& v) { grid_ = v; queue_redraw(); }
    Vector2i get_grid() const { return grid_; }
    void set_spacing(double v) { spacing_ = v; queue_redraw(); }
    double get_spacing() const { return spacing_; }

    void _draw() override {
        const Color fill(0.894f, 0.651f, 0.447f, 0.55f);
        const Color line(0.184f, 0.282f, 0.345f, 1.0f);

        if (body_type_ == 0) {
            // Drawn at the inflated radius, which is where the body will
            // actually be once the solver builds it. Drawing the authored
            // radius instead would make every over-inflated blob sit
            // slightly wrong in the editor and right in the game.
            const double drawn = radius_ * std::sqrt(pressure_ > 0.0 ? pressure_ : 1.0);
            PackedVector2Array points;
            points.resize(particle_count_);
            for (int i = 0; i < particle_count_; ++i) {
                const double a = -6.28318530718 * static_cast<double>(i) /
                                 static_cast<double>(particle_count_);
                points[i] = Vector2(static_cast<float>(std::cos(a) * drawn),
                                    static_cast<float>(std::sin(a) * drawn));
            }
            draw_colored_polygon(points, fill);
            points.push_back(points[0]);
            draw_polyline(points, line, 1.5f);
        } else if (body_type_ == 1) {
            const float w = static_cast<float>((grid_.x - 1) * spacing_);
            const float h = static_cast<float>((grid_.y - 1) * spacing_);
            draw_rect(Rect2(0, 0, w, h), fill, true);
            draw_rect(Rect2(0, 0, w, h), line, false, 1.5f);
            for (int y = 0; y < grid_.y; ++y)
                for (int x = 0; x < grid_.x; ++x)
                    draw_circle(Vector2(static_cast<float>(x * spacing_),
                                        static_cast<float>(y * spacing_)),
                                2.0f, line);
        }
    }

protected:
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("set_body_type", "v"), &PppSoftBody::set_body_type);
        ClassDB::bind_method(D_METHOD("get_body_type"), &PppSoftBody::get_body_type);
        ADD_PROPERTY(PropertyInfo(Variant::INT, "body_type", PROPERTY_HINT_ENUM,
                                  "Pressure,Lattice,Rope"),
                     "set_body_type", "get_body_type");

        ClassDB::bind_method(D_METHOD("set_particle_count", "v"), &PppSoftBody::set_particle_count);
        ClassDB::bind_method(D_METHOD("get_particle_count"), &PppSoftBody::get_particle_count);
        ADD_PROPERTY(PropertyInfo(Variant::INT, "particle_count"), "set_particle_count",
                     "get_particle_count");

        ClassDB::bind_method(D_METHOD("set_radius", "v"), &PppSoftBody::set_radius);
        ClassDB::bind_method(D_METHOD("get_radius"), &PppSoftBody::get_radius);
        ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius"), "set_radius", "get_radius");

        ClassDB::bind_method(D_METHOD("set_pressure", "v"), &PppSoftBody::set_pressure);
        ClassDB::bind_method(D_METHOD("get_pressure"), &PppSoftBody::get_pressure);
        ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pressure"), "set_pressure", "get_pressure");

        ClassDB::bind_method(D_METHOD("set_grid", "v"), &PppSoftBody::set_grid);
        ClassDB::bind_method(D_METHOD("get_grid"), &PppSoftBody::get_grid);
        ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "grid"), "set_grid", "get_grid");

        ClassDB::bind_method(D_METHOD("set_spacing", "v"), &PppSoftBody::set_spacing);
        ClassDB::bind_method(D_METHOD("get_spacing"), &PppSoftBody::get_spacing);
        ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing"), "set_spacing", "get_spacing");
    }

private:
    int body_type_ = 0;
    int particle_count_ = 18;
    double radius_ = 22.0;
    double pressure_ = 1.0;
    Vector2i grid_ = Vector2i(4, 4);
    double spacing_ = 12.0;
};

// ------------------------------------------------------------ registration

void initialize_ppp_module(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    GDREGISTER_CLASS(PppSim);
    GDREGISTER_CLASS(PppSoftBody);
}

void uninitialize_ppp_module(ModuleInitializationLevel level) {
    (void)level;
}

extern "C" {
GDExtensionBool GDE_EXPORT ppp_library_init(GDExtensionInterfaceGetProcAddress get_proc_address,
                                            const GDExtensionClassLibraryPtr p_library,
                                            GDExtensionInitialization* r_initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, p_library, r_initialization);
    init.register_initializer(initialize_ppp_module);
    init.register_terminator(uninitialize_ppp_module);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
}
