#pragma once

namespace ts {

struct GameEngine;

void move_shots(GameEngine& engine, int time);
void move_bonus(GameEngine& engine, int time);
void move_explosions(GameEngine& engine);
void move_scores(GameEngine& engine);
void move_cam(GameEngine& engine);

} // namespace ts
