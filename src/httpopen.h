/*
  ESPPC - an HTTP request that follows redirects across http and https
*/


#pragma once


#include <stdint.h>


struct esp_http_client;


// One request on the IDF's esp_http_client. open() sends method ("GET", "HEAD") to url and
// follows up to five redirects itself, including from http:// to https:// and back. https is not
// certificate checked (see sdkconfig.defaults): encrypted, not authenticated.
//
// from > 0 asks for the data from that byte on (a Range request, to resume a download): a server
// that can answers 206 with the rest, one that cannot answers 200 with everything.
class HttpRequest {
public:
  static constexpr int READ_TIMEOUT = -1;      // read(): nothing arrived for a second
  static constexpr int READ_ERROR   = -2;      // read(): the connection failed

  ~HttpRequest()                                { close(); }

  // the final status code, or a negative error (errorText())
  int  open(char const * url, char const * method = "GET", long long from = 0);

  // the body's length (Content-Length), -1 when the server does not say
  long long size() const                        { return m_size; }

  // up to len bytes of the body: the count, 0 at its end, or READ_TIMEOUT or READ_ERROR. Blocks
  // for a second at most, so a caller can keep its display going and notice a stall.
  int  read(void * buf, int len);

  void close();

  static char const * errorText(int code);

  // internal
  void header(char const * key, char const * value);

private:
  int  request(char const * url, char const * method, long long from);

  esp_http_client * m_client = nullptr;
  long long         m_size   = -1;
  char              m_location[256];
};
