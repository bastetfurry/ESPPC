/*
  PCEmulator - machine repository lists

  A repository is a URL with a "files.list" under it: CSV, one machine per line, at most
  REPO_MAXENTRIES of them. This is only the parser, with no Arduino dependencies, so it can be
  tested on the host; fetching and the list screen are in repository.cpp. The format is described
  in docs/TODO.md ("Machine repositories").
*/


#pragma once


#include <stdint.h>


constexpr int REPO_MAXENTRIES = 20;


struct RepoEntry {
  char      name[44];       // what goes into "desc", as wide as the machine list shows
  char      url[256];       // the image, a full http:// or https:// URL
  char      explain[79];    // the provider's short description, shown under the list
  char      error[48];      // why the line was rejected; empty for a usable entry
  uint16_t  ramSizeKB;
  uint8_t   videoAdapter;   // 0 = CGA, 1 = EGA, 2 = Tandy 1000, as MachineConfItem
  bool      adlib;
  bool      tandyDAC;

  bool valid() const        { return error[0] == 0; }
};


struct RepoList {
  RepoEntry entries[REPO_MAXENTRIES];
  int       count;
  bool      truncated;      // there were more lines than REPO_MAXENTRIES
};


// Parses a files.list held in text (len bytes, need not be terminated) into list.
void repoParse(char const * text, int len, RepoList * list);

// The files.list URL for what the user typed: "<base>/files.list", tolerating a trailing '/', and
// taking a URL that already ends in "files.list" as it is. False if it does not fit.
bool repoListURL(char const * base, char * out, int outSize);

// "CGA Adlib", "Tandy TandyDAC", ... for the list, the reverse of what the parser read
void repoTypeText(RepoEntry const & e, char * out, int outSize);

// The name an image is saved under on the SD card: the last path component of its URL, which is
// what getDisk() uses. Points into url.
char const * repoImageName(char const * url);

// Whether a file of this size is a standard floppy image (160K to 2.88M), so the machine gets it
// as fd0 rather than hd0. The same sizes as Machine::autoDetectDriveGeometry().
bool repoIsFloppySize(long long size);
