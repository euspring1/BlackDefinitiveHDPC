// Movie subtitles drawn over the movie (the game has none, and .xmv cannot be re-encoded): .srt files in an enabled
// mod's subtitles\ folder, named after the movie. See d3d/subtitles.cpp.
#pragma once

// Called once per frame with the render size, before the picture is presented.
void subtitles_tick(int screen_w, int screen_h);
// Drop the loaded .srt files (a mod was turned on or off).
void subtitles_reload();
