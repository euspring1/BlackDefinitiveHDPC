#pragma once

// Saved games from an Xbox (or from another language of the game) dropped into savedata\Partition1\UDATA\45410083.
void saveimport_scan();          // at start: re-signs profiles signed by another console
void install_saveimport_patch(); // after the XBE is mapped: a profile under another name is moved to the game's own
                                 // when the game looks for its profile
