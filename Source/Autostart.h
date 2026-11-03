//
//  Autostart.h — "Start with Windows": the HKCU Run value for the running exe. Windows only; on other
//  platforms read() is always off and the writers do nothing. Never called at start-up, by the tests or by -run-selftest.
//

#pragma once

#include "Engine/AutostartCommand.h"

namespace lighthost::autostart
{

State read();               // off / this exe / another copy
bool  enableForThisExe();   // writes (or overwrites) the value; false if the write failed
bool  disable();            // deletes the value; false if it failed

} // namespace lighthost::autostart
