#include "render_svg.h"

#include <cstdio>
#include <sstream>

namespace ppp {
namespace {

std::string N(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return buf;
}

std::string Rect(const AABB& r, const char* fill, const char* extra = "") {
    std::ostringstream o;
    o << "<rect x=\"" << N(r.min.x) << "\" y=\"" << N(r.min.y) << "\" width=\"" << N(r.Width())
      << "\" height=\"" << N(r.Height()) << "\" fill=\"" << fill << "\" " << extra << "/>\n";
    return o.str();
}

void DrawBodies(std::ostringstream& o, const Solver& solver, const RenderOptions& options) {
    const float r = solver.Contract().particle_radius;

    if (options.show_constraints) {
        o << "<g stroke=\"#3a4a63\" stroke-width=\"0.6\" opacity=\"0.5\">\n";
        for (const auto& c : solver.Constraints()) {
            const Vec2& a = solver.Particles().pos[static_cast<size_t>(c.a)];
            const Vec2& b = solver.Particles().pos[static_cast<size_t>(c.b)];
            o << "<line x1=\"" << N(a.x) << "\" y1=\"" << N(a.y) << "\" x2=\"" << N(b.x)
              << "\" y2=\"" << N(b.y) << "\"/>\n";
        }
        o << "</g>\n";
    }

    static const char* kFill[3] = {"#7fc8a9", "#e4a672", "#b58ec9"};
    for (size_t bi = 0; bi < solver.Bodies().size(); ++bi) {
        const SoftBody& body = solver.Bodies()[bi];
        const char* fill = kFill[static_cast<int>(body.type) % 3];

        if (body.type == BodyType::Pressure && body.ring.size() >= 3) {
            // A pressure body's outline is its ring, so draw the polygon:
            // drawing the particles alone makes an inflated blob and a
            // collapsed one look the same at a glance.
            o << "<polygon points=\"";
            for (const int idx : body.ring) {
                const Vec2& p = solver.Particles().pos[static_cast<size_t>(idx)];
                o << N(p.x) << "," << N(p.y) << " ";
            }
            o << "\" fill=\"" << fill << "\" fill-opacity=\"" << (body.sleeping ? "0.35" : "0.7")
              << "\" stroke=\"#2f4858\" stroke-width=\"1\"/>\n";
        }

        for (const int idx : body.particles) {
            const Vec2& p = solver.Particles().pos[static_cast<size_t>(idx)];
            o << "<circle cx=\"" << N(p.x) << "\" cy=\"" << N(p.y) << "\" r=\"" << N(r)
              << "\" fill=\"" << fill << "\" stroke=\"#2f4858\" stroke-width=\"0.4\"/>\n";
        }
    }
}

std::string Header(const Level& level) {
    const AABB& b = level.camera_bounds;
    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"" << N(b.min.x) << " " << N(b.min.y)
      << " " << N(b.Width()) << " " << N(b.Height()) << "\" width=\"" << N(b.Width()) << "\" height=\""
      << N(b.Height()) << "\">\n";
    o << Rect(b, "#12161d");
    return o.str();
}

void DrawLevel(std::ostringstream& o, const Level& level, const RenderOptions& options,
               const std::vector<DoorState>* doors) {
    const AABB& b = level.camera_bounds;

    if (options.show_grid) {
        o << "<g stroke=\"#1c2430\" stroke-width=\"0.5\">\n";
        for (float x = b.min.x; x <= b.max.x; x += 32.0f)
            o << "<line x1=\"" << N(x) << "\" y1=\"" << N(b.min.y) << "\" x2=\"" << N(x) << "\" y2=\""
              << N(b.max.y) << "\"/>\n";
        for (float y = b.min.y; y <= b.max.y; y += 32.0f)
            o << "<line x1=\"" << N(b.min.x) << "\" y1=\"" << N(y) << "\" x2=\"" << N(b.max.x)
              << "\" y2=\"" << N(y) << "\"/>\n";
        o << "</g>\n";
    }

    for (const auto& box : level.boxes) {
        if (box.door_id < 0) {
            o << Rect(box.rect, "#46566b", "stroke=\"#6b7f96\" stroke-width=\"0.5\"");
            continue;
        }
        bool open = false;
        if (doors != nullptr) {
            for (const auto& d : *doors)
                if (d.id == box.door_id) open = d.open;
        }
        o << Rect(box.rect, open ? "#2a3340" : "#8a6f4e",
                  open ? "stroke=\"#3d4a5c\" stroke-dasharray=\"3 3\" stroke-width=\"0.8\""
                       : "stroke=\"#c09a66\" stroke-width=\"0.8\"");
    }

    for (const auto& h : level.hazards)
        o << Rect(h, "#a8434a", "stroke=\"#d9646b\" stroke-width=\"0.8\"");

    if (options.show_plates) {
        for (const auto& p : level.plates)
            o << Rect(p.region, "#4b6b8a", "fill-opacity=\"0.45\" stroke=\"#8fb6d6\" stroke-width=\"0.8\"");
    }

    o << Rect(level.goal, "#6fcf97", "fill-opacity=\"0.35\" stroke=\"#6fcf97\" stroke-width=\"1.2\"");

    for (const auto& c : level.checkpoints) {
        o << "<circle cx=\"" << N(c.position.x) << "\" cy=\"" << N(c.position.y) << "\" r=\""
          << N(c.radius) << "\" fill=\"none\" stroke=\"#f2c14e\" stroke-width=\"1\" "
          << "stroke-dasharray=\"4 3\"/>\n";
    }
}

void DrawTrail(std::ostringstream& o, const RenderOptions& options) {
    if (options.trail.size() < 2) return;
    o << "<polyline fill=\"none\" stroke=\"#f2c14e\" stroke-width=\"1.2\" opacity=\"0.85\" points=\"";
    for (const auto& p : options.trail) o << N(p.x) << "," << N(p.y) << " ";
    o << "\"/>\n";
}

void DrawCaption(std::ostringstream& o, const Level& level, const std::string& caption) {
    if (caption.empty()) return;
    const AABB& b = level.camera_bounds;
    o << "<text x=\"" << N(b.min.x + 8.0f) << "\" y=\"" << N(b.min.y + 16.0f)
      << "\" font-family=\"monospace\" font-size=\"11\" fill=\"#9fb3c8\">";
    for (const char c : caption) {
        if (c == '<') o << "&lt;";
        else if (c == '>') o << "&gt;";
        else if (c == '&') o << "&amp;";
        else o << c;
    }
    o << "</text>\n";
}

}  // namespace

std::string RenderLevelSvg(const Level& level, const RenderOptions& options) {
    std::ostringstream o;
    o << Header(level);
    DrawLevel(o, level, options, nullptr);

    // The authored bodies, drawn as their rest shapes. Nothing has been
    // simulated, so this is the level as the author placed it.
    for (const auto& b : level.bodies) {
        switch (b.type) {
            case BodyType::Pressure:
                o << "<circle cx=\"" << N(b.position.x) << "\" cy=\"" << N(b.position.y) << "\" r=\""
                  << N(b.radius) << "\" fill=\"#e4a672\" fill-opacity=\"0.6\" stroke=\"#2f4858\"/>\n";
                break;
            case BodyType::Lattice:
                o << Rect(AABB{b.position,
                               {b.position.x + static_cast<float>(b.width - 1) * b.spacing,
                                b.position.y + static_cast<float>(b.height - 1) * b.spacing}},
                          "#7fc8a9", "fill-opacity=\"0.6\" stroke=\"#2f4858\"");
                break;
            case BodyType::Rope:
                o << "<line x1=\"" << N(b.position.x) << "\" y1=\"" << N(b.position.y) << "\" x2=\""
                  << N(b.to.x) << "\" y2=\"" << N(b.to.y)
                  << "\" stroke=\"#b58ec9\" stroke-width=\"3\"/>\n";
                break;
        }
    }

    o << Rect(AABB{{level.spawn.x - 7.0f, level.spawn.y - 12.0f},
                   {level.spawn.x + 7.0f, level.spawn.y + 12.0f}},
              "#e8e8e8", "fill-opacity=\"0.8\"");

    DrawTrail(o, options);
    DrawCaption(o, level, options.caption);
    o << "</svg>\n";
    return o.str();
}

std::string RenderSvg(const Level& level, const Sim& sim, const RenderOptions& options) {
    std::ostringstream o;
    o << Header(level);
    DrawLevel(o, level, options, &sim.Doors());
    DrawBodies(o, sim.GetSolver(), options);
    DrawTrail(o, options);

    const Player& p = sim.GetPlayer();
    o << Rect(PlayerBox(p, sim.Tuning()), "#e8e8e8",
              "stroke=\"#ffffff\" stroke-width=\"0.8\"");
    // The hand, so a grab is visible (doc 7.3 asks for a visible indicator
    // of what is grabbed).
    const Vec2 hand = HandPosition(p, sim.Tuning());
    o << "<circle cx=\"" << N(hand.x) << "\" cy=\"" << N(hand.y) << "\" r=\"2.5\" fill=\""
      << (p.grabbed_body >= 0 ? "#f2c14e" : "#7a8899") << "\"/>\n";
    if (p.grabbed_body >= 0 && p.grabbed_particle >= 0) {
        const Vec2& q = sim.GetSolver().Particles().pos[static_cast<size_t>(p.grabbed_particle)];
        o << "<line x1=\"" << N(hand.x) << "\" y1=\"" << N(hand.y) << "\" x2=\"" << N(q.x)
          << "\" y2=\"" << N(q.y) << "\" stroke=\"#f2c14e\" stroke-width=\"1\"/>\n";
    }

    DrawCaption(o, level, options.caption);
    o << "</svg>\n";
    return o.str();
}

}  // namespace ppp
