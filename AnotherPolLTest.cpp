#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <string>
#include <curl/curl.h>
#include <iostream>
#include "resources.h"
#include <vector>
#include <chrono>
#include <unistd.h> // time measurement

#include "AnotherPolLTest.h"

 /*
g++ -std=c++11 AnotherPolLTest.cpp -lpthread -lcurl -luv -o AnotherPolLTest && ./AnotherPolLTest
*/

uv_loop_t *loop;
CURLM *curl_handle = nullptr;
uv_timer_t timeout;
int downloadsLeft;

static void on_timeout(uv_timer_t *req)
{
  int running_handles;
  curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0,
                           &running_handles);
  check_multi_info();
}

void fakeTimerCB(uv_prepare_t *handle)
{
  on_timeout(nullptr);
  if (downloadsLeft == 0) {
    uv_prepare_stop(handle);
  }
}

size_t writeToString(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  char *conts = (char *)contents;
  for (int i = 0; i < nmemb; i++)
  {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

static curl_context_t *create_curl_context(curl_socket_t sockfd)
{
  curl_context_t *context;
 
  context = (curl_context_t *) malloc(sizeof(*context));
 
  context->sockfd = sockfd;
 
  uv_poll_init_socket(loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;
 
  return context;
}
 
static void curl_close_cb(uv_handle_t *handle)
{
  curl_context_t *context = (curl_context_t *) handle->data;
  free(context);
}
 
static void destroy_curl_context(curl_context_t *context)
{
  uv_close((uv_handle_t *) &context->poll_handle, curl_close_cb);
}

static CURL* createHandle(std::string url, std::string *dst, std::string etag)
{
  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());

  std::string etagHeader = "If-None-Match: \"" + etag + "\"";
  struct curl_slist *curlHeaders = nullptr;
  curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);

  return handle;
}
 
static void check_multi_info(void)
{
  char *done_url;
  CURLMsg *message;
  int pending;
  CURL *easy_handle;
 
  while((message = curl_multi_info_read(curl_handle, &pending))) {
    switch(message->msg) {
    case CURLMSG_DONE:
      /* Do not use message data after calling curl_multi_remove_handle() and
         curl_easy_cleanup(). As per curl_multi_info_read() docs:
         "WARNING: The data the returned pointer points to will not survive
         calling curl_multi_cleanup, curl_multi_remove_handle or
         curl_easy_cleanup." */
      easy_handle = message->easy_handle;
 
      curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &done_url);
      long code;
      curl_easy_getinfo(easy_handle, CURLINFO_HTTP_CODE, &code);
      std::cout << "Done. Url " << done_url << ", code " << code << "\n"; 
      if (code != 304) {
        std::cout << "ERROR: Code " << code << ", URL " << done_url << "\n";
      }
 
      on_timeout(nullptr);

      downloadsLeft--;
      curl_multi_remove_handle(curl_handle, easy_handle);
      curl_easy_cleanup(easy_handle);
      break;
 
    default:
      fprintf(stderr, "CURLMSG default\n");
      break;
    }
  }
}
 
static void curl_perform(uv_poll_t *req, int status, int events)
{
  int running_handles;
  int flags = 0;
  curl_context_t *context;
 
  if(events & UV_READABLE)
    flags |= CURL_CSELECT_IN;
  if(events & UV_WRITABLE)
    flags |= CURL_CSELECT_OUT;
 
  context = (curl_context_t *) req->data;
 
  curl_multi_socket_action(curl_handle, context->sockfd, flags,
                           &running_handles);
 
  check_multi_info();
}
  
static int start_timeout(CURLM *multi, long timeout_ms, void *userp)
{
  if(timeout_ms < 0) {
    uv_timer_stop(&timeout);
  }
  else {
    if(timeout_ms == 0)
      timeout_ms = 1; /* 0 means directly call socket_action, but we will do it
                         in a bit */
    uv_timer_start(&timeout, on_timeout, timeout_ms, 0);
  }
  return 0;
}
 
static int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp,
                  void *socketp)
{
  curl_context_t *curl_context;
  int events = 0;
 
  switch(action) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    curl_context = socketp ?
      (curl_context_t *) socketp : create_curl_context(s);
 
    curl_multi_assign(curl_handle, s, (void *) curl_context);
 
    if(action != CURL_POLL_IN)
      events |= UV_WRITABLE;
    if(action != CURL_POLL_OUT)
      events |= UV_READABLE;
 
    uv_poll_start(&curl_context->poll_handle, events, curl_perform);
    break;
  case CURL_POLL_REMOVE:
    if(socketp) {
      uv_poll_stop(&((curl_context_t*)socketp)->poll_handle);
      destroy_curl_context((curl_context_t*) socketp);
      curl_multi_assign(curl_handle, s, NULL);
    }
    break;
  default:
    abort();
  }
 
  return 0;
}
 
std::vector<std::string*> createPathsFromCS()
{
  std::vector<std::string*> vec;
  std::string *tmp = new std::string();
  for(int i = 0; i < pathsCS.size(); i++) {
    if (pathsCS[i] == ',') {
      vec.push_back(tmp);
      tmp = new std::string();
    } else {
      (*tmp) += pathsCS[i];
    }
  }
  vec.push_back(tmp);
  return vec;
}

std::vector<std::string*> createEtagsFromCS()
{
  std::vector<std::string*> vec;
  std::string *tmp = new std::string();
  for(int i = 0; i < etagsCS.size(); i++) {
    if (etagsCS[i] == ',') {
      vec.push_back(tmp);
      tmp = new std::string();
    } else {
      (*tmp) += etagsCS[i];
    }
  }
  vec.push_back(tmp);
  return vec;
}


int main(int argc, char **argv)
{

  int PATH_LIMIT = 495;
  int CONNECTION_LIMIT = 1;
  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  std::vector<std::string*> results;

  downloadsLeft = PATH_LIMIT;
  
  auto start = std::chrono::system_clock::now();

  if(curl_global_init(CURL_GLOBAL_ALL)) {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }
 
  loop = uv_default_loop();
  uv_timer_init(loop, &timeout);

  curl_handle = curl_multi_init();
  curl_multi_setopt(curl_handle, CURLMOPT_SOCKETFUNCTION, handle_socket);
  curl_multi_setopt(curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout);
  curl_multi_setopt(curl_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, CONNECTION_LIMIT);

  for(int i = 0; i < PATH_LIMIT; i++) {
    results.push_back(new std::string());
    auto handle = createHandle(*paths[i], results.back(), *etags[i]);
    curl_multi_add_handle(curl_handle, handle);
  }
  
  // uv_prepare_t fakeTimer;
  // uv_prepare_init(loop, &fakeTimer);
  // uv_prepare_start(&fakeTimer, fakeTimerCB);

  uv_run(loop, UV_RUN_DEFAULT);
  curl_multi_cleanup(curl_handle);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Result benchmark " << difference << "ms.\n";
 
  return 0;
}