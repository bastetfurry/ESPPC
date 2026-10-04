/*
  PCEmulator - "Add from Repository" on the machine selection screen (F6)
*/


#pragma once


#include "textui.h"
#include "mconf.h"


// Asks for a repository URL (url is in/out, prefilled with the last one used), fetches its
// files.list and lets the user add machines from it to mconf, saving mconfs.txt after each one.
// Returns the index of the last machine added, or -1 if none was.
int setupAddFromRepository(TextUI & ui, MachineConf & mconf, char * url, int urlSize);
