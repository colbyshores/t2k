#pragma once

#include <set>
#include <string>
#include <unordered_map>

#include <SDL2/SDL.h>

namespace ts {

// =============================================================================
// Game Actions
// =============================================================================

enum class GameAction {
    MOVE_LEFT,
    MOVE_RIGHT,
    // Gameplay vertical (the warp rounds' flight axis). Shares the physical
    // up/down inputs with MENU_UP/MENU_DOWN — InputHandler::update() aliases
    // them so the arrows/stick keep their documented meaning everywhere.
    MOVE_UP,
    MOVE_DOWN,
    SHOOT,
    JUMP,
    TREMOR,
    ZAPPER,
    PAUSE,
    CONFIRM,
    CANCEL,
    MENU_UP,
    MENU_DOWN,
    MENU_LEFT,
    MENU_RIGHT,
    QUIT,
    NEXT_LEVEL,
    TOGGLE_VIEW,
};

// Joystick axis deadzone as fraction of range.
constexpr float JOYSTICK_DEADZONE = 0.33f;

// =============================================================================
// Input State
// =============================================================================

struct InputState {
    std::set<GameAction> held;
    std::set<GameAction> pressed;
    bool quit_requested = false;
};

// =============================================================================
// Input Handler
// =============================================================================

class InputHandler {
public:
    InputHandler();
    ~InputHandler();

    /// Process all pending events and return the current input state.
    /// Call exactly once per frame, before processing game logic.
    InputState update();

    /// Check if any action button was just pressed.
    /// Used for "press any button to continue" prompts.
    bool anyButtonPressed(const InputState& state) const;

private:
    void readJoystick(InputState& state);

    std::unordered_map<SDL_Scancode, GameAction> key_bindings_;
    SDL_Joystick* joystick_ = nullptr;
    std::set<GameAction> prev_held_;
};

} // namespace ts
