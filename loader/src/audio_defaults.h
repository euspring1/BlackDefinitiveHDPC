#pragma once
// The audio volumes kept in black.ini (audio_defaults.cpp): first start sound effects 60 %, music 40 %.

void audio_defaults_init();  // at start, after g_cfg is set
void audio_defaults_frame(); // every frame, on the game thread (D3DDevice_Swap)
