/*
  PCEmulator - host setup screens on the text UI: the machine editor, the file picker, the
  Files browser and disk image creation. These replace FabGL's uiApp dialogs.
*/


#pragma once


#include "textui.h"
#include "mconf.h"


// Edits the configuration at idx. isNew means it was just inserted and is dropped again if the
// user leaves without saving (the caller reloads the file). Returns true when it was saved.
bool setupEditMachine(TextUI & ui, MachineConf & mconf, int idx);

// Picks a file. dir (absolute, ie "/SD/disks") and filename are in/out. With allowNew the user can
// also type a name that does not exist yet, which is how "Create Disk" names its image.
bool setupFileDialog(TextUI & ui, char const * title, char * dir, int maxDir, char * filename, int maxFilename, bool allowNew);

// The Files browser: navigate, make folders, rename, delete, copy between folders.
void setupFilesBrowser(TextUI & ui, char const * startDir);

// Asks for a disk type and a filename, then writes the image.
void setupCreateDiskImage(TextUI & ui);
