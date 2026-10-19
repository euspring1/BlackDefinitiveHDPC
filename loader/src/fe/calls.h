// Calls from the front end's menu scripts into the port (names starting with "PC").
#pragma once

void install_fe_calls(); // after the XBE is mapped

// The port changed a setting from the pointer-driving pages (input/bindings.cpp): show it on the page at once.
void fe_refresh();
