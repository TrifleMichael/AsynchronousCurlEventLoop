#include <curl/curl.h>
#include <iostream>
// #include <stdlib.h>
#include <curl/curl.h>
#include <vector>
#include "resources.h"
#include <chrono>
#include <unistd.h> // time measurement
#include <unordered_map>

#include <uv.h>

/*
g++ -std=c++11 socketTest.cpp -lpthread -lcurl -luv -o socketTest && ./socketTest
*/

CURLM* multiHandle;
int counter = 0;
std::unordered_map<std::string, std::string> urlEtagMap;
uv_loop_t *uvMainLoop = nullptr;
int downloads;
std::unordered_map<curl_socket_t, struct curl_context_s *> socketContextMap;

struct curl_context_s
{
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
  int action;
};

void curlCloseCB(uv_handle_t *handle)
{
  // std::cout << "curlCloseCB\n";
  struct curl_context_s *context = (struct curl_context_s *)handle->data;
  free(context);
}

void destroyCurlContext(struct curl_context_s *context)
{
  // std::cout << "destroyCurlContext\n";
  socketContextMap.erase(context->sockfd);
  uv_close((uv_handle_t *)&context->poll_handle, curlCloseCB);
}


int doneRequests = 0;
void checkMultiInfo()
{
  // std::cout << "checkMultiInfo\n";

  char *done_url;
  CURLMsg *message;
  int pending;
  CURL *easy_handle;

  while ((message = curl_multi_info_read(multiHandle, &pending)))
  {
    switch (message->msg)
    {
    case CURLMSG_DONE:
    {
      /* Do not use message data after calling curl_multi_remove_handle() and
        curl_easy_cleanup(). As per curl_multi_info_read() docs:
        "WARNING: The data the returned pointer points to will not survive
        calling curl_multi_cleanup, curl_multi_remove_handle or
        curl_easy_cleanup." */
      
      easy_handle = message->easy_handle;
      curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &done_url);

      long code;
      curl_easy_getinfo(easy_handle, CURLINFO_RESPONSE_CODE, &code);
      if (code != 200 && code != 304) {
        std::cout << "ERROR.   Code: " << code << ", url: " << done_url << "\n";
      }

      // std::cout << "Done: " << ++doneRequests << "\n";

      // delete event;
      curl_multi_remove_handle(multiHandle, easy_handle);
      downloads--;
      // curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &data); 

      // curl_easy_getinfo(easy_handle,  CURLINFO_RESPONSE_CODE, data->codeDestination);      
    }
    break;

    default:
      fprintf(stderr, "CURLMSG default\n");
      break;
    }
  }
}

void curl_perform(uv_poll_t *req, int status, int events)
{
  std::cout << "curl_perform\n";

  int running_handles;
  int flags = 0;
  if (events & UV_READABLE)
    flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE)
    flags |= CURL_CSELECT_OUT;

  auto event = (struct curl_context_s *)req->data;
  curl_multi_socket_action(multiHandle, event->sockfd, flags,
                           &running_handles);
  checkMultiInfo();
}

struct curl_context_s* createContext(curl_socket_t sockfd, int events)
{
    // std::cout << "createCurlContext\n";
  if (socketContextMap.find(sockfd) == socketContextMap.end()) {
    struct curl_context_s *context;

    context = (struct curl_context_s *)malloc(sizeof(*context));
    context->sockfd = sockfd;
    context->action = events;

    uv_poll_init_socket(uvMainLoop, &context->poll_handle, sockfd);
    context->poll_handle.data = context;
    socketContextMap[sockfd] = context;

    return context;
  }
  return socketContextMap[sockfd];
}

void socketCB(CURL *easy,      /* easy handle */
                    curl_socket_t s, /* socket */
                    int what,        /* describes the socket */
                    void *userp,     /* private callback pointer */
                    void *socketp)  /* private socket pointer */
{
  // std::cout << "socketCB\n";
  struct curl_context_s *curl_context;
  int events = 0;

  switch (what)
  {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:

    if (what != CURL_POLL_IN)
      events |= UV_WRITABLE;
    if (what != CURL_POLL_OUT)
      events |= UV_READABLE;

    curl_context = socketp ? (struct curl_context_s *)socketp : createContext(s, events);
    // if(events != curl_context->action) {
    //   curl_context->action = events;
    //   uv_poll_start(&curl_context->poll_handle, events, curl_perform);
    // }
    // if (events != curl_context->action) std::cout << "Not the same!\n";
    // if (events == curl_context->action) std::cout << "Same!\n";
    // std::cout << &curl_context->poll_handle << " " << events << " " << curl_perform << "\n";
    // std::cout << "Poll started " << &curl_context->poll_handle << "\n";
    curl_multi_assign(multiHandle, s, (void *)curl_context);
    std::cout << "Start\n";
    uv_poll_start(&curl_context->poll_handle, events, curl_perform);
    std::cout << "End\n";

    // std::cout << "Poll start\n";
    break;
  case CURL_POLL_REMOVE:
    if (socketp)
    {
      // std::cout << "Poll stopped " << &((curl_context_s *)socketp)->poll_handle << "\n";
      uv_poll_stop(&((curl_context_s *)socketp)->poll_handle);
      destroyCurlContext((curl_context_s *)socketp);
      curl_multi_assign(multiHandle, s, NULL);
    }
    break;
  default:
    abort();
  }
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

size_t writeToString2(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  // std::cout << "writeToString2\n";
  char *conts = (char *)contents;
  for (int i = 0; i < nmemb; i++)
  {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

CURL* createHandle(std::string url, std::string* dst, std::string* headerDst)
{
  // std::cout << "createHandle\n";
  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString2);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  if (headerDst) {
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, writeToString2);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, headerDst);
  }
  return handle;
}

void cleanHandles(std::vector<CURL*> handles) {
  // std::cout << "cleanHandles\n";
  for(auto handle : handles) {
    curl_easy_cleanup(handle);
  }
}

std::string extractETAG(std::string headers)
{
  // std::cout << "extractETAG\n";
  auto etagLine = headers.find("ETag");
  auto etagStart = headers.find("\"", etagLine)+1;
  auto etagEnd = headers.find("\"", etagStart+1);
  return headers.substr(etagStart, etagEnd - etagStart);
}



/*
    ------ UV LOOP ---------
*/

bool uvQuit = false;

void onUVClose(uv_handle_t* handle)
{
  // std::cout << "onUVClose\n";
  // if (handle != NULL)
  // {
  //   delete handle;
  // }
}

void onTimeout(uv_timer_t *req)
{
  // std::cout << "onTimeout\n";
  int running_handles;
  curl_multi_socket_action(multiHandle, CURL_SOCKET_TIMEOUT, 0,
                           &running_handles);
  checkMultiInfo();
}

int startTimeout(CURLM *multi, long timeout_ms, void *userp)
{
  // std::cout << "startTimeout\n";
  auto timeout = (uv_timer_t *)userp;

  if (timeout_ms < 0)
  {
    uv_timer_stop(timeout);
  }
  else
  {
    if (timeout_ms == 0)
      timeout_ms = 1; /* 0 means directly call socket_action, but we will do it
                        in a bit */
    uv_timer_start(timeout, onTimeout, timeout_ms, 0);
  }
  return 0;
}

void checkCB(uv_prepare_t *handle)
{
  // std::cout << "checkCB\n";
  int running_handles;
  curl_multi_socket_action(multiHandle, CURL_SOCKET_TIMEOUT, 0, &running_handles);
  uv_prepare_stop(handle);
  // uv_close((uv_handle_t*)handle, onUVClose);
}

void closeHandles(uv_handle_t* handle, void* arg)
{
  if (!uv_is_closing(handle))
    uv_close(handle, onUVClose);
}

void checkIfEndedCB(uv_prepare_t *handle)
{
  // std::cout << "checkIfEndedCB\n";
  if (downloads == 0) {
    uv_walk(uvMainLoop, closeHandles, NULL);
  }
}

void uvLoop()
{
  // std::cout << "uvLoop\n";
  uvMainLoop = uv_default_loop();

  uv_timer_t timeout;
  uv_timer_init(uvMainLoop, &timeout);
  curl_multi_setopt(multiHandle, CURLMOPT_TIMERFUNCTION, startTimeout);
  curl_multi_setopt(multiHandle, CURLMOPT_TIMERDATA, &timeout);

  uv_prepare_t kickstart;
  uv_prepare_init(uvMainLoop, &kickstart);
  uv_prepare_start(&kickstart, checkCB);

  uv_prepare_t checkIfEnded;
  uv_prepare_init(uvMainLoop, &checkIfEnded);
  uv_prepare_start(&checkIfEnded, checkIfEndedCB);

  uv_run(uvMainLoop, UV_RUN_DEFAULT);
}

void initializeMultiHandle(int MAX_CONNECTIONS)
{
  // std::cout << "initializeMultiHandle\n";
  multiHandle = curl_multi_init();
  curl_multi_setopt(multiHandle, CURLMOPT_SOCKETFUNCTION, socketCB);
  curl_multi_setopt(multiHandle, CURLMOPT_SOCKETDATA, multiHandle);
  curl_multi_setopt(multiHandle, CURLMOPT_MAX_TOTAL_CONNECTIONS, MAX_CONNECTIONS);
}

curl_socket_t openSocketFunction(void *clientp, curlsocktype purpose, struct curl_sockaddr *address)
{
  // std::cout << "Opening socket\n";
  return socket(address->family, address->socktype, address->protocol);
}

/*
    ------ BENCHMARKS ---------
*/

void testDownload(int TEST_SIZE, bool sequential, int MAX_CONNECTIONS, bool uv)
{
  downloads = TEST_SIZE;
  initializeMultiHandle(MAX_CONNECTIONS);
  auto paths = createPathsFromCS();
  // std::cout << *paths[0] << "\n";
  std::vector<std::string *> results;
  std::vector<std::string *> headers;
  std::vector<CURL*> handles;

  auto start = std::chrono::system_clock::now();

  // paths.size()
  for(int i = 0; i < TEST_SIZE; i++) {
    results.push_back(new std::string());
    headers.push_back(new std::string());
    handles.push_back(createHandle(*paths[i], results.back(), headers.back()));
    curl_easy_setopt(handles.back(), CURLOPT_OPENSOCKETFUNCTION, openSocketFunction); 
    curl_multi_add_handle(multiHandle, handles.back());
    if (sequential) {
      if (uv) {
        uvLoop();
      }
    }
  }

  if (!sequential)
  {
    if (uv) {
      uvLoop();
    }
  }

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();


  // std::cout << "Dst: " << dst << "\n";

  for(int i = 0; i < TEST_SIZE; i++) {
    urlEtagMap[*paths[i]] = extractETAG(*headers[i]);
  }

  std::cout << "Download - " << difference << "ms.\n";
  cleanHandles(handles);
  curl_multi_cleanup(multiHandle);
}

int64_t testValidate(int TEST_SIZE, bool sequential, int MAX_CONNECTIONS, bool uv)
{
  downloads = TEST_SIZE;
  initializeMultiHandle(MAX_CONNECTIONS);

  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  std::vector<std::string*> results2;

  auto start2 = std::chrono::system_clock::now();

  std::vector<CURL*> handles2;
  for(int i = 0; i < TEST_SIZE; i++) {
    results2.push_back(new std::string());
    handles2.push_back(createHandle(*paths[i], results2.back(), nullptr));
    curl_easy_setopt(handles2.back(), CURLOPT_OPENSOCKETFUNCTION, openSocketFunction);  

    std::string etagHeader = "If-None-Match: \"" + *etags[i] + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handles2.back(), CURLOPT_HTTPHEADER, curlHeaders);

    curl_multi_add_handle(multiHandle, handles2.back());
    if (sequential) {
      if (uv) {
        uvLoop();
      }
    }
  }

  if (!sequential) {
    if (uv) {
      uvLoop();
    }
  }

  auto end2 = std::chrono::system_clock::now();

  for (auto handle : handles2)
  {
    long code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    if (code != 304)
    {
      std::cout << "INVALID CODE: " << code << "\n";
    }
  }
  cleanHandles(handles2);
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();

  std::cout << "Validate - " << difference2 << "ms.\n";

  // std::cout << "LINEAR TEST:          download - " << difference << "ms, Check validity - " <<  difference2 << "ms.\n";
  curl_multi_cleanup(multiHandle);
  return difference2;
}


int main()
{

  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
  }


  int PATH_SIZE = 10;
  int REPEATS = 1;
  int MAX_CONNECTIONS = 4;
  bool sequential = false;
  bool useLibUV = true;


  // testDownload(PATH_SIZE, sequential, MAX_CONNECTIONS, useLibUV);

  int64_t totalTime = 0;
  for(int i = 0; i < REPEATS; i++) {
    totalTime += testValidate(PATH_SIZE, sequential, MAX_CONNECTIONS, useLibUV);
  }
  std::cout << "Average validate time = " << totalTime/REPEATS << "ms\n";
  std::cout << "Average time * connections " << MAX_CONNECTIONS*(totalTime/REPEATS) << "\n";



  curl_global_cleanup();
}


// Printing ETags
  // std::cout << "\n\n\n\n\n\n\n\n\n\n\n\n";
  // auto paths = createPathsFromCS();
  // for(auto path : paths) {
  //   std::cout << urlEtagMap[*path] << ",";
  // }

// Testing optimal number of parallel connections
  // for (int j = 1; j < 50; j++)
  // {
  //   int64_t totalTime = 0;
  //   for (int i = 0; i < REPEATS; i++)
  //   {
  //     totalTime += testValidate(PATH_SIZE, sequential, j);
  //   }
  //   std::cout << "Average validate time = " << totalTime / REPEATS << "ms\n";
  //   std::cout << "Average time * connections " << j * (totalTime / REPEATS) << "\n";
  //   std::cout << "J " << j << "\n-------------------------------\n";
  // }