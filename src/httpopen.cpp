/*
  ESPPC - an HTTP request that follows redirects across http and https. See httpopen.h.
*/


#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <string>

#include "esp_http_client.h"

#include "httpopen.h"


static esp_err_t onEvent(esp_http_client_event_t * e)
{
  if (e->event_id == HTTP_EVENT_ON_HEADER)
    ((HttpRequest *) e->user_data)->header(e->header_key, e->header_value);
  return ESP_OK;
}


void HttpRequest::header(char const * key, char const * value)
{
  if (!strcasecmp(key, "Location"))
    snprintf(m_location, sizeof(m_location), "%s", value);
}


int HttpRequest::request(char const * url, char const * method, long long from)
{
  m_location[0] = 0;
  m_size        = -1;

  esp_http_client_config_t cfg = { };
  cfg.url                  = url;
  cfg.method               = !strcmp(method, "HEAD") ? HTTP_METHOD_HEAD : HTTP_METHOD_GET;
  cfg.timeout_ms           = 10000;            // connecting, TLS and the response header
  cfg.disable_auto_redirect = true;            // its redirects keep the scheme; open() does them
  cfg.buffer_size          = 2048;             // per transport read, from internal RAM (see downloadURL())
  cfg.event_handler        = onEvent;
  cfg.user_data            = this;
  m_client = esp_http_client_init(&cfg);
  if (!m_client)
    return -ESP_ERR_NO_MEM;

  if (from > 0) {
    char range[40];
    snprintf(range, sizeof(range), "bytes=%lld-", from);
    esp_http_client_set_header(m_client, "Range", range);
  }

  esp_err_t err = esp_http_client_open(m_client, 0);
  if (err != ESP_OK)
    return -err;
  int64_t len = esp_http_client_fetch_headers(m_client);
  if (len < 0)
    return -ESP_FAIL;
  if (!esp_http_client_is_chunked_response(m_client) && esp_http_client_get_content_length(m_client) >= 0)
    m_size = esp_http_client_get_content_length(m_client);

  // the body comes a second at most at a time, see read()
  esp_http_client_set_timeout_ms(m_client, 1000);
  return esp_http_client_get_status_code(m_client);
}


int HttpRequest::open(char const * url, char const * method, long long from)
{
  std::string u = url;
  for (int hop = 0; ; ++hop) {
    close();
    int code = request(u.c_str(), method, from);
    bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
    if (!redirect || !m_location[0] || hop == 5)
      return code;

    // an absolute path keeps the scheme and host
    if (m_location[0] == '/') {
      size_t host = u.find("://");
      size_t path = host == std::string::npos ? std::string::npos : u.find('/', host + 3);
      u = (path == std::string::npos ? u : u.substr(0, path)) + m_location;
    } else
      u = m_location;
  }
}


int HttpRequest::read(void * buf, int len)
{
  if (!m_client)
    return READ_ERROR;
  int n = esp_http_client_read(m_client, (char *) buf, len);
  if (n == -ESP_ERR_HTTP_EAGAIN)
    return READ_TIMEOUT;
  return n < 0 ? READ_ERROR : n;
}


void HttpRequest::close()
{
  if (m_client) {
    esp_http_client_close(m_client);
    esp_http_client_cleanup(m_client);
    m_client = nullptr;
  }
}


char const * HttpRequest::errorText(int code)
{
  return code < 0 ? esp_err_to_name(-code) : "no error";
}
