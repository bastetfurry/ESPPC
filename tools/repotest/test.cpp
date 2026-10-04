// Host test for the repository list parser (src/repolist.cpp).
//   g++ -std=c++17 -Wall -Isrc tools/repotest/test.cpp src/repolist.cpp -o /tmp/repotest && /tmp/repotest
#include <stdio.h>
#include <string.h>
#include "repolist.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static RepoList L;
static void parse(char const * s) { repoParse(s, strlen(s), &L); }

int main()
{
  // the example from docs/TODO.md, header included
  parse("\"Name\", \"Full-Imagefile-URL\", \"Memory in KB\", \"Machine-Type\", \"Explanation\"\n"
        "\"FreeDOS 1.3\", \"http://foo.test/pcimages/img/freedos.img\", \"640\", \"CGA Adlib\", \"FreeDOS only\"\r\n"
        "\"Tandy DeskMate\", \"http://foo.test/pcimages/img/deskmate.img\", \"640\", \"Tandy TandyDAC\", \"DeskMate in autoboot\"\n");
  CHECK(L.count == 2 && !L.truncated);
  CHECK(L.entries[0].valid() && !strcmp(L.entries[0].name, "FreeDOS 1.3"));
  CHECK(!strcmp(L.entries[0].url, "http://foo.test/pcimages/img/freedos.img"));
  CHECK(L.entries[0].ramSizeKB == 640 && L.entries[0].videoAdapter == 0 && L.entries[0].adlib && !L.entries[0].tandyDAC);
  CHECK(!strcmp(L.entries[0].explain, "FreeDOS only"));
  CHECK(L.entries[1].valid() && L.entries[1].videoAdapter == 2 && L.entries[1].tandyDAC && !L.entries[1].adlib);
  CHECK(!strcmp(repoImageName(L.entries[1].url), "deskmate.img"));

  // unquoted, comments, blank lines, no header, explanation missing, case
  parse("# a comment\n\n   \nPlain, https://x.test/a/b.img , 704 , ega adlib tandydac\n");
  CHECK(L.count == 1 && L.entries[0].valid());
  CHECK(!strcmp(L.entries[0].name, "Plain") && !strcmp(L.entries[0].url, "https://x.test/a/b.img"));
  CHECK(L.entries[0].ramSizeKB == 704 && L.entries[0].videoAdapter == 1 && L.entries[0].adlib && L.entries[0].tandyDAC);
  CHECK(L.entries[0].explain[0] == 0);

  // quoting: commas and doubled quotes inside quotes
  parse("\"Say \"\"Hi\"\", ok\", \"http://h/x.img\", 640, CGA, \"one, two\"\n");
  CHECK(L.count == 1 && L.entries[0].valid());
  CHECK(!strcmp(L.entries[0].name, "Say \"Hi\", ok"));
  CHECK(!strcmp(L.entries[0].explain, "one, two"));

  // rejected lines stay in the list, with the reason
  parse("A, http://h/a.img, 600, CGA\n"
        "B, http://h/b.img, 768, CGA\n"
        "C, http://h/c.img, 640, VGA\n"
        "D, ftp://h/d.img, 640, CGA\n"
        "E, http://h/e.img, 640\n"
        "F, http://h/f.img, 640, CGA EGA\n"
        "G, http://h/g.img, 640, Adlib\n"
        "H, http://h/, 640, CGA\n"
        "I, http://h, 640, CGA\n"
        ", http://h/j.img, 640, CGA\n"
        "K, http://h/k.img, 64x, CGA\n"
        "L, http://h/a b.img, 640, CGA\n");
  CHECK(L.count == 12);
  for (int i = 0; i < L.count; ++i) {
    CHECK(!L.entries[i].valid());
    printf("  rejected %-2s %s\n", L.entries[i].name, L.entries[i].error);
  }

  // more than 20 lines
  {
    char big[4000] = "";
    for (int i = 0; i < 25; ++i)
      sprintf(big + strlen(big), "M%d, http://h/m%d.img, 640, CGA\n", i, i);
    parse(big);
    CHECK(L.count == REPO_MAXENTRIES && L.truncated);
    CHECK(!strcmp(L.entries[19].name, "M19"));
  }

  // explanation truncated to 78, long name cut to the list width
  parse("NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN, http://h/x.img, 640, CGA, "
        "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE\n");
  CHECK(L.count == 1 && L.entries[0].valid() && strlen(L.entries[0].explain) == 78 && strlen(L.entries[0].name) == 43);

  // no trailing newline, BOM
  parse("\xEF\xBB\xBFName,URL,Mem,Type\nX, http://h/x.img, 128, Tandy");
  CHECK(L.count == 1 && L.entries[0].valid() && L.entries[0].ramSizeKB == 128);

  // list URL
  char u[128];
  CHECK(repoListURL("http://foo.test/pcimages/1", u, sizeof(u)) && !strcmp(u, "http://foo.test/pcimages/1/files.list"));
  CHECK(repoListURL("http://foo.test/pcimages/1/", u, sizeof(u)) && !strcmp(u, "http://foo.test/pcimages/1/files.list"));
  CHECK(repoListURL(" http://foo.test/x/files.list ", u, sizeof(u)) && !strcmp(u, "http://foo.test/x/files.list"));
  CHECK(!repoListURL("http://a-very-long-host.test/path", u, 20));

  CHECK(repoIsFloppySize(1474560) && repoIsFloppySize(163840) && !repoIsFloppySize(10 * 1048576) && !repoIsFloppySize(-1));

  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures != 0;
}
