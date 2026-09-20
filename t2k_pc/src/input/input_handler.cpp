#include "input_handler.h"

#include <cstdio>

namespace ts {

// Default keyboard bindings
static const std::unordered_map<SDL_Scancode, GameAction> DEFAULT_KEY_BINDINGS = {
    {SDL_SCANCODE_LEFT,   GameAction::MOVE_LEFT},
    {SDL_SCANCODE_RIGHT,  GameAction::MOVE_RIGHT},
    {SDL_SCANCODE_A,      GameAction::SHOOT},
    {SDL_SCANCODE_S,      GameAction::JUMP},
    {SDL_SCANCODE_W,      GameAction::TREMOR},
    {SDL_SCANCODE_Q,      GameAction::ZAPPER},
    {SDL_SCANCODE_P,      GameAction::PAUSE},
    {SDL_SCANCODE_ESCAPE, GameAction::CANCEL},
    {SDL_SCANCODE_RETURN, GameAction::CONFIRM},
    {SDL_SCANCODE_UP,     GameAction::MENU_UP},
    {SDL_SCANCODE_DOWN,   GameAction::MENU_DOWN},
    {SDL_SCANCODE_N,      GameAction::NEXT_LEVEL},
    {SDL_SCANCODE_V,      GameAction::TOGGLE_VIEW},
};

InputHandler::InputHandler()
    : key_bindings_(DEFAULT_KEY_BINDINGS)
{
    // Initialize joystick subsystem if not already done
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    }

    // Detect and open first joystick if available
    int num_joysticks = SDL_NumJoysticks();
    if (num_joysticks > 0) {
        joystick_ = SDL_JoystickOpen(0);
        if (joystick_) {
            std::printf("Gamepad detected: %s\n", SDL_JoystickName(joystick_));
        }
    }
}

InputHandler::~InputHandler() {
    if (joystick_) {
        SDL_JoystickClose(joystick_);
        joystick_ = nullptr;
    }
}

InputState InputHandler::update() {
    InputState state;

    // Process SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                state.quit_requested = true;
                break;
            default:
                break;
        }
    }

    // Read current keyboard state
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    for (const auto& [scancode, action] : key_bindings_) {
        if (keys[scancode]) {
            state.held.insert(action);
        }
    }

    // Read gamepad state
    if (joystick_) {
        readJoystick(state);
    }

    // Vertical is ONE physical axis (arrow keys / stick Y) with two consumers:
    // menus read MENU_UP/MENU_DOWN, the warp rounds' flight reads
    // MOVE_UP/MOVE_DOWN. Alias rather than rebind, so the user-facing keys
    // stay exactly as the boot text documents them.
    if (state.held.count(GameAction::MENU_UP))   state.held.insert(GameAction::MOVE_UP);
    if (state.held.count(GameAction::MENU_DOWN)) state.held.insert(GameAction::MOVE_DOWN);

    // Calculate pressed from delta with previous frame
    for (const auto& action : state.held) {
        if (prev_held_.find(action) == prev_held_.end()) {
            state.pressed.insert(action);
        }
    }
    prev_held_ = state.held;

    return state;
}

void InputHandler::readJoystick(InputState& state) {
    if (!joystick_) return;

    // Axes
    int num_axes = SDL_JoystickNumAxes(joystick_);
    if (num_axes >= 1) {
        float x_axis = SDL_JoystickGetAxis(joystick_, 0) / 32767.0f;
        if (x_axis < -JOYSTICK_DEADZONE) {
            state.held.insert(GameAction::MOVE_LEFT);
        } else if (x_axis > JOYSTICK_DEADZONE) {
            state.held.insert(GameAction::MOVE_RIGHT);
        }
    }

    if (num_axes >= 2) {
        float y_axis = SDL_JoystickGetAxis(joystick_, 1) / 32767.0f;
        if (y_axis < -JOYSTICK_DEADZONE) {
            state.held.insert(GameAction::MENU_UP);
        } else if (y_axis > JOYSTICK_DEADZONE) {
            state.held.insert(GameAction::MENU_DOWN);
        }
    }

    // Buttons (Xbox-style mapping)
    int num_buttons = SDL_JoystickNumButtons(joystick_);
    struct ButtonMapping {
        int button;
        GameAction action;
    };
    // the reference build joy b1..b4 (the reference source:275-278): b1=Shoot, b2=Tremor, b3=Jump, b4=Zapper.
    static const ButtonMapping button_map[] = {
        {0, GameAction::SHOOT},
        {1, GameAction::TREMOR},
        {2, GameAction::JUMP},
        {3, GameAction::ZAPPER},
    };

    for (const auto& mapping : button_map) {
        if (mapping.button < num_buttons && SDL_JoystickGetButton(joystick_, mapping.button)) {
            state.held.insert(mapping.action);
        }
    }
}

bool InputHandler::anyButtonPressed(const InputState& state) const {
    return !state.pressed.empty();
}

} // namespace ts
