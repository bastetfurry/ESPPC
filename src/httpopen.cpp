/*
  PCEmulator - an HTTP request that follows redirects across http and https. See httpopen.h.
*/


#include "httpopen.h"


int httpOpen(HTTPClient & http, char const * url, char const * method, long long from)
{
  String u = url;

  for (int hop = 0; ; ++hop) {
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    // getStreamPtr() is the raw socket and does not decode chunked transfer encoding, which would
    // end up in the data. HTTP/1.0 keeps servers from sending chunked bodies.
    http.useHTTP10(true);
    if (!http.begin(u))
      return HTTPC_ERROR_CONNECTION_REFUSED;
    if (from > 0) {
      char range[40];
      snprintf(range, sizeof(range), "bytes=%lld-", from);
      http.addHeader("Range", range);
    }
    int code = http.sendRequest(method);

    bool redirect = code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
                    code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT ||
                    code == HTTP_CODE_PERMANENT_REDIRECT;
    String loc = http.getLocation();
    if (!redirect || !loc.length() || hop == 5)
      return code;
    http.end();

    // an absolute path keeps the scheme and host
    if (loc[0] == '/') {
      int host = u.indexOf("://");
      int path = host < 0 ? -1 : u.indexOf('/', host + 3);
      u = (path < 0 ? u : u.substring(0, path)) + loc;
    } else
      u = loc;
  }
}
