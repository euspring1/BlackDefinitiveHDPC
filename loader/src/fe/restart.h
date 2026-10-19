// Restart the game after a mod's files change (the game reads them only at start). See fe/restart.cpp.
#pragma once

// Called once per frame: relaunches the game when a mod changed and the main menu is up.
void restart_poll();
