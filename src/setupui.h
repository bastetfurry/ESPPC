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


// Settings of the host rather than of a machine. main.cpp loads and saves them (Preferences, the
// ESP32's NVS, next to the Wi-Fi credentials); the page below only edits them. Adding a setting
// means a field here, a row in setupAdvancedSettings() and its load/save in main.cpp.
struct HostSettings {
  static constexpr char const * DEFAULT_NTP_SERVER = "pool.ntp.org";
  static constexpr char const * DEFAULT_TIME_ZONE  = "CET-1CEST,M3.5.0,M10.5.0/3";   // POSIX TZ
  static constexpr int          DEFAULT_AUTOBOOT   = 6;
  static constexpr int          DEFAULT_DRIVE_LED  = 0;
  char ntpServer[64];
  char timeZone[64];      // a POSIX TZ string, as setenv("TZ") takes it; empty = the default
  int  autoBootSeconds;   // machine selection countdown: -1 = wait, 0 = boot at once, else seconds
  int  driveLed;          // on-screen drive LED: 0 = off, 1 = one for all drives, 2 = one per drive
};

// The "Advanced Settings" page. Returns true when the user saved; settings holds the new values
// then, and is left alone otherwise.
bool setupAdvancedSettings(TextUI & ui, HostSettings & settings);
