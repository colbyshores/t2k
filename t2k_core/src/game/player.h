#pragma once

namespace ts {

struct GameEngine;

void player_shoot(GameEngine& engine);
void player_jump(GameEngine& engine);
void player_move_left(GameEngine& engine, bool pressed);
void player_move_right(GameEngine& engine, bool pressed);
void player_tremor(GameEngine& engine, int time);
void player_zapper(GameEngine& engine, int time);
void move_jump(GameEngine& engine, int time);

} // namespace ts
