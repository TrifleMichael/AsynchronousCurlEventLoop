#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <string>
#include <curl/curl.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <unistd.h> // time measurement

 /*
g++ -std=c++11 AnotherPolLTest.cpp -lpthread -lcurl -luv -o AnotherPolLTest && ./AnotherPolLTest
*/
 
typedef struct curl_context_s {
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
} curl_context_t;

static void on_timeout(uv_timer_t *req);
void fakeTimerCB(uv_prepare_t *handle);
size_t writeToString(void *contents, size_t size, size_t nmemb, std::string *dst);
static curl_context_t *create_curl_context(curl_socket_t sockfd);
static void curl_close_cb(uv_handle_t *handle);
static void destroy_curl_context(curl_context_t *context);
static CURL* createHandle(std::string url, std::string *dst, std::string etag);
static void check_multi_info(void);
static void curl_perform(uv_poll_t *req, int status, int events);
static int start_timeout(CURLM *multi, long timeout_ms, void *userp);
static int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp);
std::vector<std::string*> createPathsFromCS();
std::vector<std::string*> createEtagsFromCS();