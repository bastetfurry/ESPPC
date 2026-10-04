/*
  PCEmulator - an HTTP request that follows redirects across http and https
*/


#pragma once


#include <HTTPClient.h>


// Sends method ("GET", "HEAD") to url with HTTP/1.0 and follows up to five redirects, including
// from http:// to https:// and back, which HTTPClient's own following refuses (setURL() keeps the
// protocol), so an http:// image on a server that moved to https failed with a 301. https is not
// certificate checked: without a CA it is encrypted, not authenticated.
//
// Returns the final status code, or a negative HTTPClient error. The response is ready on http
// either way (getSize(), getStreamPtr()); the caller calls http.end().
//
// from > 0 asks for the data from that byte on (a Range request, to resume a download): a server
// that can answers 206 with the rest, one that cannot answers 200 with everything.
int httpOpen(HTTPClient & http, char const * url, char const * method = "GET", long long from = 0);
