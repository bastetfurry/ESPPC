/*
  PCEmulator - machine repository lists, the parser. See repolist.h.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "repolist.h"


// copies at most size - 1 characters and always terminates
static void copyField(char * dest, int size, char const * src)
{
  snprintf(dest, size, "%s", src);
}


// Splits one line into fields. Fields are separated by commas, with optional blanks around them,
// and may be quoted; inside quotes a comma is ordinary and "" is a literal quote. Each field is
// written terminated into buf, one after the other, and pointed at from fields[]. Returns the
// number of fields.
static int splitLine(char const * line, int len, char * buf, int bufSize, char const ** fields, int maxFields)
{
  int n = 0, i = 0, o = 0;

  while (n < maxFields) {
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
      ++i;
    fields[n++] = buf + o;
    if (i < len && line[i] == '"') {
      ++i;
      while (i < len) {
        if (line[i] == '"') {
          if (i + 1 < len && line[i + 1] == '"') {
            if (o < bufSize - 1)
              buf[o++] = '"';
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        if (o < bufSize - 1)
          buf[o++] = line[i];
        ++i;
      }
      // anything between the closing quote and the comma is dropped
      while (i < len && line[i] != ',')
        ++i;
    } else {
      int start = o;
      while (i < len && line[i] != ',') {
        if (o < bufSize - 1)
          buf[o++] = line[i];
        ++i;
      }
      while (o > start && (buf[o - 1] == ' ' || buf[o - 1] == '\t'))
        --o;
    }
    if (o < bufSize)
      buf[o++] = 0;
    else
      buf[bufSize - 1] = 0;
    if (i >= len)
      break;
    ++i;      // the comma
  }
  return n;
}


// "CGA", "EGA" or "Tandy", then optionally "Adlib" and "TandyDAC", in any order. Unknown words
// reject the line, so that a typo does not quietly give a different machine.
static bool parseType(char const * s, RepoEntry * e)
{
  int video = -1;
  e->adlib    = false;
  e->tandyDAC = false;

  while (*s) {
    while (*s == ' ' || *s == '\t')
      ++s;
    if (!*s)
      break;
    char word[24];
    int n = 0;
    while (*s && *s != ' ' && *s != '\t') {
      if (n < (int) sizeof(word) - 1)
        word[n++] = *s;
      ++s;
    }
    word[n] = 0;

    int v = -1;
    if      (!strcasecmp(word, "CGA"))      v = 0;
    else if (!strcasecmp(word, "EGA"))      v = 1;
    else if (!strcasecmp(word, "Tandy"))    v = 2;
    else if (!strcasecmp(word, "Adlib"))    e->adlib = true;
    else if (!strcasecmp(word, "TandyDAC")) e->tandyDAC = true;
    else {
      snprintf(e->error, sizeof(e->error), "unknown type \"%.20s\"", word);
      return false;
    }
    if (v >= 0) {
      if (video >= 0 && video != v) {
        copyField(e->error, sizeof(e->error), "more than one video adapter");
        return false;
      }
      video = v;
    }
  }

  if (video < 0) {
    copyField(e->error, sizeof(e->error), "no video adapter (CGA, EGA, Tandy)");
    return false;
  }
  e->videoAdapter = video;
  return true;
}


static void parseLine(char const * line, int len, RepoEntry * e)
{
  memset(e, 0, sizeof(*e));

  char buf[600];
  char const * f[5] = { "", "", "", "", "" };
  int n = splitLine(line, len, buf, sizeof(buf), f, 5);

  copyField(e->name, sizeof(e->name), f[0]);
  copyField(e->url, sizeof(e->url), f[1]);
  copyField(e->explain, sizeof(e->explain), f[4]);

  if (n < 4) {
    copyField(e->error, sizeof(e->error), "needs name, URL, memory and type");
    return;
  }
  if (!e->name[0]) {
    copyField(e->error, sizeof(e->error), "no name");
    return;
  }
  if (strncasecmp(f[1], "http://", 7) && strncasecmp(f[1], "https://", 8)) {
    copyField(e->error, sizeof(e->error), "the image is not an http(s) URL");
    return;
  }
  if (strpbrk(f[1], " \t\"")) {
    copyField(e->error, sizeof(e->error), "blanks or quotes in the image URL");
    return;
  }
  if (strlen(f[1]) >= sizeof(e->url)) {
    copyField(e->error, sizeof(e->error), "the image URL is too long");
    return;
  }
  char const * host = strstr(e->url, "://") + 3;
  if (!strchr(host, '/') || !repoImageName(e->url)[0]) {
    copyField(e->error, sizeof(e->error), "the image URL names no file");
    return;
  }

  char * end;
  long kb = strtol(f[2], &end, 10);
  if (end == f[2] || *end || kb < 64 || kb > 704 || kb % 64) {
    snprintf(e->error, sizeof(e->error), "memory \"%.10s\" is not 64 to 704 KB by 64", f[2]);
    return;
  }
  e->ramSizeKB = (uint16_t) kb;

  parseType(f[3], e);
}


void repoParse(char const * text, int len, RepoList * list)
{
  list->count     = 0;
  list->truncated = false;

  int i = 0;
  bool first = true;
  while (i < len) {
    int start = i;
    while (i < len && text[i] != '\n')
      ++i;
    int end = i;
    ++i;
    if (end > start && text[end - 1] == '\r')
      --end;
    // UTF-8 byte order mark, as some editors write it
    if (first && end - start >= 3 && !memcmp(text + start, "\xEF\xBB\xBF", 3))
      start += 3;

    int s = start;
    while (s < end && (text[s] == ' ' || text[s] == '\t'))
      ++s;
    if (s == end || text[s] == '#')
      continue;

    // a first line whose name is "Name" is the header
    if (first) {
      first = false;
      char buf[64];
      char const * f[1];
      splitLine(text + s, end - s, buf, sizeof(buf), f, 1);
      if (!strcasecmp(f[0], "Name"))
        continue;
    }

    if (list->count == REPO_MAXENTRIES) {
      list->truncated = true;
      break;
    }
    parseLine(text + s, end - s, &list->entries[list->count++]);
  }
}


bool repoListURL(char const * base, char * out, int outSize)
{
  while (*base == ' ')
    ++base;
  int len = strlen(base);
  while (len > 0 && base[len - 1] == ' ')
    --len;

  static char const LISTNAME[] = "files.list";
  constexpr int NAMELEN = sizeof(LISTNAME) - 1;

  int n;
  if (len >= NAMELEN && !strncasecmp(base + len - NAMELEN, LISTNAME, NAMELEN))
    n = snprintf(out, outSize, "%.*s", len, base);
  else {
    while (len > 0 && base[len - 1] == '/')
      --len;
    n = snprintf(out, outSize, "%.*s/%s", len, base, LISTNAME);
  }
  return n > 0 && n < outSize;
}


void repoTypeText(RepoEntry const & e, char * out, int outSize)
{
  static char const * VIDEO[] = { "CGA", "EGA", "Tandy" };
  snprintf(out, outSize, "%s%s%s", VIDEO[e.videoAdapter % 3], e.adlib ? " Adlib" : "", e.tandyDAC ? " TandyDAC" : "");
}


char const * repoImageName(char const * url)
{
  char const * s = strrchr(url, '/');
  return s ? s + 1 : url;
}


bool repoIsFloppySize(long long size)
{
  static long long const SIZES[] = { 163840, 184320, 327680, 368640, 737280, 1228800, 1474560, 2949120 };
  for (auto s : SIZES)
    if (s == size)
      return true;
  return false;
}
