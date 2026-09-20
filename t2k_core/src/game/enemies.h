#pragma once

namespace ts {

struct GameEngine;

void move_enemies(GameEngine& engine, int time);
void move_embrios(GameEngine& engine, int time);

} // namespace ts
