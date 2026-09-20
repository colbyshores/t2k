#pragma once

namespace ts {

struct GameEngine;

void move_tremor(GameEngine& engine, int time);
void move_ai_droid(GameEngine& engine);
void move_zapper(GameEngine& engine, int time);

} // namespace ts
