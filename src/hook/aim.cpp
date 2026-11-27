// Mouse aim point (issue #15), opt-in: [aim] track_pointer=1.
//
// Stock MW4 treats mouse motion as a turn *speed*: each frame's motion is
// added to an accumulator (-1..1) that is fed to the torso as its rate
// command and drained over time. The torso keeps turning after the mouse
// stops, fast motion saturates at full rate (a flick lands short), and the
// drain has a per-frame minimum, so the result depends on the frame rate.
//
// With track_pointer the mouse moves an aim point instead: an angle relative
// to the legs, resolution independent. The torso turns toward it at the
// mech's own twist rate and stops on it, so a movement of the same size
// lands in the same place however fast it was made. The aim point stops at
// the mech's twist / pitch limits; on a torso that turns all the way round
// (Vulture) it keeps going, at most half a turn ahead of the torso. Turning
// the legs carries it along, as in the stock game. Torso keys and Center
// Torso keep working; the aim point
// follows the torso while they are used. Aiming never turns the 'Mech: the
// game's automatic centering (options.ini autotorsocenter: the legs turn
// toward the torso after a delay) is off in aim mode (issue #31; controls.cpp
// runs the drive step with it off). With [aim] auto_center=1 it keeps
// working and the aim point stays where it is in the world while the legs
// catch up.
//
// Applies only with the mouse on and the Mouse X axis set to Torso
// (Options > Controls); otherwise the stock code runs unchanged.
//
// Game side (3.1c, located by unique patterns):
//   0x0054EDA0  per-frame mouse step of the player's mech controller
//               (thiscall, one double: time). Runs before the keyboard drive
//               step 0x0054F2A0 (controls.cpp), which also computes torso
//               auto-centering.
//   Controller:
//     +0x4AC / +0x4B0  float  mouse motion X / Y this frame (counts)
//     +0x17C           float  mouse sensitivity (Options > Controls slider)
//     +0x554 / +0x558  float  stock mouse accumulators
//     +0x18C           int    Mouse X axis: 0 none, 1 turning, 2 torso
//     +0x4BD           byte   mouse enabled
//     +0x2E0 / +0x2E4  float  torso pitch / yaw rate commands (-1..1)
//     +0x464 +0x468 +0x46C +0x470  int  torso right / left / up / down keys held
//     +0x6D0           int    Center Torso To Legs in progress
//     +0x38C           mech
//   Mech:
//     +0x440           vector; element 0 is the torso state:
//                        +0x19C / +0x1A0  torso yaw / pitch (rad, + = left / up)
//                        +0x1CC / +0x1D0  applied yaw / pitch rate (-1..1)
//                        +0x78            sub-object; virtual [1] returns the
//                                         'Mech's torso data (below)
//   Torso data (the torso update 0x005F3C30 clamps with these, issue #37):
//     +0x634 / +0x638  float  twist / pitch rate (rad/s)
//     +0x63C / +0x640  float  twist / pitch limit (rad, symmetric). The
//                      update wraps the yaw to +-pi first, so a twist limit
//                      of pi or more never clamps: the torso turns all the
//                      way round (2pi on the Vulture).
//   Measured: Vulture 2pi / 50 degrees, Hunchback 120 / 50 degrees.
//   (mech +0x76C / +0x7C8, read before #37, hold 80 / 30 degrees on both,
//   so every 'Mech's aim point stopped there; used only if the torso data
//   cannot be read.)
//   Mouse step +0x22E: cmp byte [mousey], 0 -- options.ini [special commands]
//               mousey: mouse Y pitches the torso.
#include "hook.h"

#include <cmath>

namespace mw4hook {
namespace {

constexpr size_t kDeltaX = 0x4AC;
constexpr size_t kDeltaY = 0x4B0;
constexpr size_t kSensitivity = 0x17C;
constexpr size_t kAccX = 0x554;
constexpr size_t kAccY = 0x558;
constexpr size_t kMouseXAxis = 0x18C;
constexpr size_t kMouseEnabled = 0x4BD;
constexpr size_t kPitchCmd = 0x2E0;
constexpr size_t kYawCmd = 0x2E4;
constexpr size_t kKeysFirst = 0x464;  // four int counters
constexpr size_t kCentering = 0x6D0;
constexpr size_t kLegsTurnCmd = 0x2DC;  // legs' turn rate command (-1..1, + = left)
constexpr size_t kMech = 0x38C;
constexpr size_t kMechParts = 0x440;
constexpr size_t kMechTwistLimit = 0x76C;  // fallback only, see above
constexpr size_t kMechPitchLimit = 0x7C8;
constexpr size_t kTorsoData = 0x78;  // sub-object; virtual [1] returns the torso data
constexpr size_t kDataTwistLimit = 0x63C;
constexpr size_t kDataPitchLimit = 0x640;
constexpr size_t kHeadingCos = 0xF4;
constexpr size_t kHeadingSin = 0xFC;
constexpr size_t kTorsoYaw = 0x19C;
constexpr size_t kTorsoPitch = 0x1A0;
constexpr size_t kTorsoYawRate = 0x1CC;
constexpr size_t kTorsoPitchRate = 0x1D0;

constexpr int kAxisTorso = 2;
// Radians per mouse count at the default sensitivity slider (0.01): about
// 160 degrees of twist for 3 inches at 800 counts per inch.
constexpr float kRadiansPerSensitivity = 0.116f;
constexpr float kDefaultRate = 0.9f;  // rad/s at full command, until measured
// Closer than this to the aim point counts as on it (rad; one pixel at
// 1024x768 is about 0.0009 rad).
constexpr float kOnTarget = 0.0005f;
constexpr float kPi = 3.14159265f;
// A torso that turns all the way round: the aim point may lead the torso by
// at most this much (just under half a turn, so the way round is never in
// doubt), so it does not pile up turns the torso then has to catch up on.
constexpr float kMaxLead = kPi - 0.05f;

using MouseStep_t = void(__thiscall*)(void*, double);
MouseStep_t g_mouse_step = nullptr;
volatile LONG g_gyro = 0;  // Options > Game > Gyro Aim
const unsigned char* g_mousey = nullptr;
bool g_installed = false;

// One axis of the aim point.
struct Axis {
    float target = 0.0f;
    float rate = kDefaultRate;  // measured rad/s per unit command
    float last_angle = 0.0f;
    float last_applied = 0.0f;
};

struct State {
    char* torso = nullptr;  // the torso the aim point belongs to
    bool active = false;    // the mouse step ran in aim mode this frame
    float pending_x = 0.0f;  // mouse counts since the last drive step
    float pending_y = 0.0f;
    double last_now = 0.0;
    Axis yaw;
    Axis pitch;
    float heading = 0.0f;  // legs' heading last frame
    bool have_heading = false;
    // Gyro Aim's prediction of the legs' turn (issue #33).
    float last_dt = 0.0f;    // the previous frame's time step
    float legs_cmd = 0.0f;   // the previous frame's legs' turn command
    float legs_rate = 0.0f;  // learned legs' turn rate, rad/s at full command (0 = unknown)
    float predicted = 0.0f;  // legs' turn predicted (and taken off) for the last frame
    // For the marker (read by the ddraw proxy through Mw4AimMarker).
    volatile float marker_yaw = 0.0f;    // aim point minus torso, rad
    volatile float marker_pitch = 0.0f;
    volatile DWORD marker_tick = 0;      // GetTickCount of the last update
} g;

template <typename T>
T& At(char* base, size_t offset) {
    return *reinterpret_cast<T*>(base + offset);
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The torso state of the controller's mech, or nullptr.
char* Torso(char* self, char** mech_out) {
    __try {
        char* mech = At<char*>(self, kMech);
        if (!mech) {
            return nullptr;
        }
        char** begin = At<char**>(mech, kMechParts);
        char** end = At<char**>(mech, kMechParts + 4);
        if (!begin || end <= begin) {
            return nullptr;
        }
        *mech_out = mech;
        return begin[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool AimMode(char* self) {
    return GetConfig().aim_track_pointer && At<int>(self, kMouseXAxis) == kAxisTorso &&
           At<unsigned char>(self, kMouseEnabled) != 0;
}

void __fastcall Hook_MouseStep(char* self, void*, double now) {
    g.active = AimMode(self);
    if (g.active) {
        // Take this frame's motion and keep the stock path from turning the
        // torso: no motion, empty accumulators.
        g.pending_x += At<float>(self, kDeltaX);
        g.pending_y += At<float>(self, kDeltaY);
        At<float>(self, kDeltaX) = 0.0f;
        At<float>(self, kDeltaY) = 0.0f;
        At<float>(self, kAccX) = 0.0f;
        At<float>(self, kAccY) = 0.0f;
    } else {
        g.pending_x = 0.0f;
        g.pending_y = 0.0f;
        g.torso = nullptr;
    }
    g_mouse_step(self, now);
}

float WrapAngle(float a);

// Rate command that brings `angle` to `axis.target` without overshoot: full
// rate while far away, then proportional, reaching it in about tau.
float Steer(Axis& axis, float angle, float applied, double dt) {
    if (dt > 0.0 && std::fabs(axis.last_applied) > 0.3f) {
        // Learn the mech's rate from the last frame's response (wrapped: a
        // torso that turns all the way round crosses +-pi).
        const float observed = std::fabs(WrapAngle(angle - axis.last_angle) /
                                         static_cast<float>(dt) / axis.last_applied);
        if (observed > 0.1f && observed < 6.0f) {
            axis.rate += (observed - axis.rate) * 0.2f;
        }
    }
    axis.last_angle = angle;
    axis.last_applied = applied;
    // On target: exactly 0. The proportional tail never reaches 0 by itself.
    // The game starts the torso twist sound (sfx_torso.wav, event 0x17) for
    // any yaw command other than exactly 0.0 (0x0054E0AA) but only moves the
    // torso for |cmd| >= 0.0001, so the leftover command kept the sound
    // playing with the torso still (issue #32). kOnTarget is under a pixel.
    const float error = axis.target - angle;
    if (std::fabs(error) < kOnTarget) {
        return 0.0f;
    }
    const float tau = static_cast<float>(dt > 0.0 ? (dt * 2.0 > 0.06 ? dt * 2.0 : 0.06) : 0.06);
    return Clamp(error / (axis.rate * tau), -1.0f, 1.0f);
}

// The legs' heading (rad, + = left, like the torso yaw) from the mech's
// rotation matrix: mech+0xF4 = cos, mech+0xFC = sin (mirrored at +0x11C /
// +0x114). Found by diffing the mech before and after a held turn.
bool Heading(char* mech, float* out) {
    __try {
        const float c = At<float>(mech, kHeadingCos);
        const float s = At<float>(mech, kHeadingSin);
        if (!(c == c) || !(s == s) || std::fabs(c * c + s * s - 1.0f) > 0.05f) {
            return false;
        }
        *out = std::atan2(s, c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float WrapAngle(float a) {
    while (a > 3.14159265f) {
        a -= 6.2831853f;
    }
    while (a < -3.14159265f) {
        a += 6.2831853f;
    }
    return a;
}

struct Limits {
    float twist;
    float pitch;
    bool full_turn;  // the torso turns all the way round
    bool from_data;  // read from the torso data (else the fallback)
};

bool Sane(float v) { return v > 0.05f && v < 100.0f; }  // also false for NaN

// The twist / pitch limits the game's torso update clamps with: the torso
// data the update gets the same way (0x005F3C65: [torso+0x78] virtual [1],
// no arguments). This runs in the game's own frame, on its thread.
Limits TorsoLimits(char* torso, char* mech) {
    Limits out{1.3963f, 0.5236f, false, false};
    __try {
        char* sub = torso + kTorsoData;
        void** vtable = *reinterpret_cast<void***>(sub);
        using GetData = char*(__thiscall*)(void*);
        char* data = vtable ? reinterpret_cast<GetData>(vtable[1])(sub) : nullptr;
        if (data) {
            const float twist = At<float>(data, kDataTwistLimit);
            const float pitch = At<float>(data, kDataPitchLimit);
            if (Sane(twist) && Sane(pitch)) {
                out.twist = twist;
                out.pitch = pitch;
                out.from_data = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!out.from_data) {
        __try {
            const float twist = At<float>(mech, kMechTwistLimit);
            const float pitch = At<float>(mech, kMechPitchLimit);
            if (Sane(twist)) {
                out.twist = twist;
            }
            if (Sane(pitch)) {
                out.pitch = pitch;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // The update wraps the yaw to +-pi before it clamps, so a limit of pi or
    // more never stops the torso.
    out.full_turn = out.twist >= kPi;
    if (out.pitch > 1.5f) {
        out.pitch = 1.5f;
    }
    return out;
}

}  // namespace

// Called by the drive hook (controls.cpp) after the game's drive step, which
// computed the torso keys and auto-centering into the rate commands.
void AimAfterDrive(char* self, double now) {
    if (!g.active) {
        return;
    }
    char* mech = nullptr;
    char* torso = Torso(self, &mech);
    if (!torso) {
        return;
    }
    const float yaw = At<float>(torso, kTorsoYaw);
    const float pitch = At<float>(torso, kTorsoPitch);
    // The commands the game's own input step left this frame (keys, and the
    // joystick if it drives them); aim mode replaces them below. The drive
    // step recomputes them every frame, so a nonzero value here is a held
    // key or a moved joystick axis, not last frame's aim.
    const float input_yaw = At<float>(self, kYawCmd);
    const float input_pitch = At<float>(self, kPitchCmd);
    // A joystick Y (pitch) or Z (twist) axis moves the torso through the
    // game's own command. Let that axis through; the aim point follows the
    // torso while the stick is moved (issue #40 follow-up: the mouse aim used
    // to replace the command, so the joystick could not pitch).
    const bool game_pitch = std::fabs(input_pitch) > 1e-4f;
    const bool game_yaw = std::fabs(input_yaw) > 1e-4f;
    static bool pitch_logged = false;
    if (game_pitch && !pitch_logged) {
        pitch_logged = true;
        Log("aim: joystick pitch %+.2f: the game drives pitch, the aim follows\n", input_pitch);
    } else if (!game_pitch) {
        pitch_logged = false;
    }
    double dt = g.torso == torso ? now - g.last_now : 0.0;
    if (dt < 0.0 || dt > 0.25) {
        dt = 0.0;
    }
    g.last_now = now;
    const float prev_dt = g.last_dt;  // the time step the legs' last turn took
    g.last_dt = static_cast<float>(dt);
    if (g.torso != torso) {
        // New mech or mission: start at the torso's current aim.
        g.torso = torso;
        g.yaw = Axis{yaw, kDefaultRate, yaw, 0.0f};
        g.pitch = Axis{pitch, kDefaultRate, pitch, 0.0f};
        g.have_heading = false;
        g.legs_cmd = 0.0f;
        g.legs_rate = 0.0f;
        g.predicted = 0.0f;
    }
    // How far the legs turned since the last frame (rad, + = left).
    float heading = 0.0f;
    float legs_turn = 0.0f;
    if (Heading(mech, &heading)) {
        if (g.have_heading) {
            legs_turn = WrapAngle(heading - g.heading);
            if (std::fabs(legs_turn) > 0.5f) {
                legs_turn = 0.0f;  // a jump (respawn, teleport), not a turn
                g.predicted = 0.0f;
            }
        }
        g.heading = heading;
        g.have_heading = true;
    } else {
        g.have_heading = false;
    }
    // The legs turn in the frame's update, after this step, by their turn
    // rate x their turn command (+0x2DC, set by A/D and the Center Legs
    // commands) x the frame's time; measured: 0.957 rad/s on the test
    // Archer, the same at every command and frame time. Learn the rate from
    // the last frame (it changes with the mech and its speed).
    const float legs_cmd = At<float>(self, kLegsTurnCmd);
    const float prev_legs_cmd = g.legs_cmd;
    g.legs_cmd = legs_cmd;
    if (legs_turn != 0.0f && std::fabs(prev_legs_cmd) > 0.05f && prev_dt > 0.0f) {
        const float k = legs_turn / (prev_legs_cmd * prev_dt);
        if (k > 0.05f && k < 10.0f) {
            g.legs_rate = k;
        }
    }

    const float sensitivity =
        At<float>(self, kSensitivity) * kRadiansPerSensitivity * GetConfig().aim_sensitivity;
    const float dx = g.pending_x;
    const float dy = g.pending_y;
    g.pending_x = 0.0f;
    g.pending_y = 0.0f;
    const bool pitch_by_mouse = !g_mousey || *g_mousey != 0;

    // Torso keys or Center Torso: the game drives the torso; the aim point
    // follows it.
    bool game_drives = At<int>(self, kCentering) != 0;
    for (int i = 0; i < 4; ++i) {
        game_drives = game_drives || At<int>(self, kKeysFirst + 4 * i) > 0;
    }
    if (game_drives) {
        g.predicted = 0.0f;
        g.yaw.target = yaw;
        g.pitch.target = pitch;
        g.yaw.last_angle = yaw;
        g.pitch.last_angle = pitch;
        g.yaw.last_applied = 0.0f;
        g.pitch.last_applied = 0.0f;
        g.marker_yaw = 0.0f;
        g.marker_pitch = 0.0f;
        g.marker_tick = GetTickCount();
        return;
    }

    // The game's own limits for this 'Mech (issue #37: the Vulture's torso
    // turns all the way round and pitches 50 degrees; an 80 / 30 degree
    // field read before stopped the aim point there).
    const Limits limits = TorsoLimits(torso, mech);
    const float twist_limit = limits.twist;
    const float pitch_limit = limits.pitch;
    // Moves the yaw aim point by `delta`. Within the twist limit, or for a
    // torso that turns all the way round, at most kMaxLead from the torso
    // angle `ref` either way (the angles wrap at +-pi there, as the game's).
    auto move_yaw_target = [&](float delta, float ref) {
        if (limits.full_turn) {
            g.yaw.target =
                ref + Clamp(WrapAngle(g.yaw.target - ref) + delta, -kMaxLead, kMaxLead);
        } else {
            g.yaw.target = Clamp(g.yaw.target + delta, -twist_limit, twist_limit);
        }
    };

    // Mouse right turns the torso right (yaw down); mouse up pitches up.
    move_yaw_target(-dx * sensitivity, yaw);
    if (pitch_by_mouse) {
        g.pitch.target = Clamp(g.pitch.target - dy * sensitivity, -pitch_limit, pitch_limit);
    }

    float feed_forward = 0.0f;
    float steer_yaw = yaw;
    const bool gyro = g_gyro != 0;
    if (gyro) {
        // Gyro Aim (issue #17): whatever turns the legs (A/D, auto-centering),
        // the torso keeps its direction in the world, like a gyro-stabilized
        // turret: the legs' turn is taken off the torso's twist angle
        // directly, so it holds however fast the legs turn (a counter-rotation
        // command is capped at the mech's twist rate and fell behind on a
        // held turn). The aim point moves with it. At the twist limit both
        // stop and the torso is carried with the legs. Mouse aiming still
        // twists at the mech's own rate.
        //
        // The legs turn later in the frame, after this step, and the frame is
        // drawn after that. Taking off only the turn measured since the last
        // step left the drawn aim one frame of leg turn off; that amount
        // changes with every frame's length, so the view shook while A/D
        // was held (issue #33). So this frame's turn is predicted from the
        // legs' turn command and taken off now, and whatever the last
        // prediction missed is taken off as well.
        float predict = 0.0f;
        if (g.legs_rate > 0.0f && dt > 0.0) {
            predict = g.legs_rate * legs_cmd * static_cast<float>(dt);
        }
        const float correction = legs_turn - g.predicted + predict;
        g.predicted = predict;
        if (GetConfig().debug_automation && (legs_turn != 0.0f || correction != 0.0f)) {
            // drawn_world: the torso's world direction in the frame just drawn.
            Log("gyro: t=%.4f dt=%.4f turn=%+.5f legs_cmd=%+.3f drawn_world=%+.5f correction=%+.5f "
                "predict=%+.5f rate=%.3f\n",
                now, dt, legs_turn, legs_cmd, WrapAngle(heading + yaw), correction, predict,
                g.legs_rate);
        }
        if (correction != 0.0f) {
            steer_yaw = limits.full_turn ? WrapAngle(yaw - correction)
                                         : Clamp(yaw - correction, -twist_limit, twist_limit);
            At<float>(torso, kTorsoYaw) = steer_yaw;
            // Keep the twist-rate learning to the torso's own motion.
            g.yaw.last_angle += WrapAngle(steer_yaw - yaw);
        }
        move_yaw_target(-correction, steer_yaw);
    } else {
        g.predicted = 0.0f;
        // Only with [aim] auto_center=1 (otherwise the drive step ran with
        // auto-centering off and the yaw command is 0 here; explicit Center
        // commands returned above). Auto-centering turns the legs toward
        // the torso and, in the same step, counter-rotates the torso (its
        // command is what the drive step left in the yaw command; the mouse
        // contributes nothing there in aim mode). Keep that counter-rotation
        // and move the aim point with it, so the aim stays put in the world
        // while the legs catch up. Without it the legs would chase a torso
        // that turns along with them.
        feed_forward = At<float>(self, kYawCmd);
        if (dt > 0.0 && feed_forward != 0.0f) {
            move_yaw_target(feed_forward * g.yaw.rate * static_cast<float>(dt), yaw);
        }
    }
    float yaw_cmd = input_yaw;
    if (game_yaw) {
        // The joystick (or a key) is twisting the torso: leave its command
        // alone and keep the aim point on the torso.
        g.yaw.target = yaw;
        g.yaw.last_angle = yaw;
        g.yaw.last_applied = 0.0f;
        g.marker_yaw = 0.0f;
    } else {
        yaw_cmd = Clamp(feed_forward + Steer(g.yaw, steer_yaw, At<float>(torso, kTorsoYawRate), dt),
                        -1.0f, 1.0f);
        At<float>(self, kYawCmd) = yaw_cmd;
        g.marker_yaw = limits.full_turn ? WrapAngle(g.yaw.target - steer_yaw)
                                        : g.yaw.target - steer_yaw;
    }
    float pitch_cmd = input_pitch;
    if (game_pitch) {
        // The joystick's Y axis is pitching: leave its command alone and keep
        // the aim point on the torso, so releasing the stick holds the aim.
        g.pitch.target = pitch;
        g.pitch.last_angle = pitch;
        g.pitch.last_applied = 0.0f;
        g.marker_pitch = 0.0f;
    } else if (pitch_by_mouse) {
        pitch_cmd = Steer(g.pitch, pitch, At<float>(torso, kTorsoPitchRate), dt);
        At<float>(self, kPitchCmd) = pitch_cmd;
        g.marker_pitch = g.pitch.target - pitch;
    } else {
        g.marker_pitch = 0.0f;
    }
    g.marker_tick = GetTickCount();

    if (GetConfig().debug_automation) {
        static double logged = 0.0;
        if (dx != 0.0f || dy != 0.0f || now - logged >= 0.05) {
            logged = now;
            Log("aim: t=%.3f d=%+.0f,%+.0f target=%+.4f,%+.4f torso=%+.4f,%+.4f cmd=%+.3f,%+.3f "
                "in=%+.3f,%+.3f rate=%.2f,%.2f limit=%.4f,%.4f%s%s heading=%+.4f world=%+.4f "
                "gyro=%d cmd0=%d\n",
                now, dx, dy, g.yaw.target, g.pitch.target, yaw, pitch, yaw_cmd, pitch_cmd,
                input_yaw, input_pitch, g.yaw.rate, g.pitch.rate, twist_limit, pitch_limit,
                limits.full_turn ? " full_turn" : "", limits.from_data ? "" : " (fallback)",
                g.heading, WrapAngle(g.heading + yaw), gyro ? 1 : 0,
                yaw_cmd == 0.0f ? 1 : 0);  // 0: the torso twist sound plays (issue #32)
        }
    }
}

// Exported to the ddraw proxy, which draws the aim-point marker. Returns -1
// when the marker is off ([aim] track_pointer or marker = 0), 1 with the aim
// point's offset from the torso aim (rad; yaw + = left, pitch as the torso's
// pitch) while aim mode is in use in a mission, else 0.
extern "C" int WINAPI Mw4AimMarker(float* yaw_offset, float* pitch_offset) {
    if (!GetConfig().aim_track_pointer || !GetConfig().aim_marker) {
        return -1;
    }
    if (!g.active || GetTickCount() - g.marker_tick > 200) {
        return 0;  // not in a mission, or aim mode is not in use
    }
    // Always shown in aim mode, also when the reticle is on the aim point.
    const float y = g.marker_yaw;
    const float p = g.marker_pitch;
    if (yaw_offset) {
        *yaw_offset = y;
    }
    if (pitch_offset) {
        *pitch_offset = p;
    }
    return 1;
}

bool AimSuppressesAutoCenter(char* controller) {
    if (!g_installed || GetConfig().aim_auto_center) {
        return false;
    }
    __try {
        return AimMode(controller);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GyroAimEnabled() { return g_gyro != 0; }

void SetGyroAim(bool enabled) {
    InterlockedExchange(&g_gyro, enabled ? 1 : 0);
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = len ? wcsrchr(path, L'\\') : nullptr;
    if (slash) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"mw4hook.ini");
        WritePrivateProfileStringW(L"aim", L"gyro", enabled ? L"1" : L"0", path);
    }
    Log("aim: gyro aim %s\n", enabled ? "on" : "off");
}

bool InstallAimHooks() {
    if (g_installed) {
        return true;
    }
    InterlockedExchange(&g_gyro, GetConfig().aim_gyro ? 1 : 0);
    unsigned char* step =
        FindUnique("mouse_step", "83 EC 18 53 55 56 8B F1 33 DB 57 38 9E 0C 09 00 00 0F 85");
    if (!step) {
        return false;  // not unpacked yet; retry
    }
    g_installed = true;
    // cmp byte ptr [mousey], bl at +0x22E (38 1D <address>).
    if (step[0x22E] == 0x38 && step[0x22F] == 0x1D) {
        g_mousey = *reinterpret_cast<const unsigned char**>(step + 0x230);
    }
    // sub esp,18h; push ebx; push ebp = 5 bytes, position independent.
    InstallDetour(step, 5, reinterpret_cast<void*>(&Hook_MouseStep),
                  reinterpret_cast<void**>(&g_mouse_step));
    Log("aim: mouse step hook %s (track_pointer %d, sensitivity %.2f, gyro %d, mousey flag %s)\n",
        g_mouse_step ? "ok" : "FAILED", GetConfig().aim_track_pointer ? 1 : 0,
        GetConfig().aim_sensitivity, static_cast<int>(g_gyro), g_mousey ? "found" : "not found");
    return true;
}

}  // namespace mw4hook
