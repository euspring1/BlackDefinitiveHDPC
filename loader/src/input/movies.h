// Skip any movie with A, START or Enter, also ones the game would not let you skip yet (input/movies.cpp).
#pragma once

#include <cstdint>

// Once per pad poll on the game thread, with the Xbox pad state the game gets and whether Enter is held.
void movies_poll(uint16_t buttons, const uint8_t analog[8], bool enter);
// A story movie (not a looping menu background) is playing right now: for the FMV 4:3 pillarbox.
bool movie_playing_once();
