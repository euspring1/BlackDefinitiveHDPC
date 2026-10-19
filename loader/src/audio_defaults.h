#pragma once
// First-run audio volumes (audio_defaults.cpp): sound effects 60 %, music 40 % until the first profile is saved.

void audio_defaults_init();  // at start, after g_cfg is set
void audio_defaults_frame(); // every frame, on the game thread (D3DDevice_Swap)
