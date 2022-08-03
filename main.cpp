#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <curl/curl.h>
#include <string>
#include <iostream>
#include <string.h>   //strcpy
#include <functional> // std::ref
#include <thread>     // get_id
#include <vector>
#include <unordered_map>

#include <chrono> // time measurement
#include <unistd.h> // time measurement

/*
g++ -std=c++11 main.cpp -lpthread -lcurl -luv -o main && ./main
*/

/*
TODO:
- Keep loop working until a closing signal comes
- Fix refreshing handle timers
- Properly closing curl handles

- reusing socket errors can happen - handle by trying new socket, and pass error as last resort

- Maybe better interface for requesting downloads?
- multiple uv loop threads

- sometimes clearing multihandle breaks

*/


uint64_t curlBuffer = 5000; // miliseconds durning which handle will be left open after last call
uv_loop_t *loop;
CURLM *curl_handle;
uv_timer_t timeout;


typedef struct curl_context_s
{
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
} curl_context_t;

typedef struct CurlHandleContext
{
  bool inUse;
  int index;
  CURL* curlHandle;
  uv_timer_t timerHandle;
} CurlHandleContext;

typedef struct UvTimerHandleData
{
  bool refresh = true;
  bool inUse = true;
  // CurlHandleContext *curlHandleContext = nullptr;
} UvTimerHandleData;

std::vector<std::unordered_map<std::string, std::string*>*> urlContentMapQueue;
std::vector<int> queueStatus;

std::vector<CurlHandleContext*> handleVector;


void timerCallback(uv_timer_t *handle)
{
  UvTimerHandleData* data = (UvTimerHandleData*)(handle->data);

  if (!data->inUse) {
    if (data->refresh) {
      data->refresh = false;
      std::cout << "Handle about to expire\n";
    } else {
      uv_timer_stop(handle);
      std::cout << "Handle expired\n";
      return;
    }
  }

  if (data->refresh) {
    uv_timer_again(handle);
    std::cout << "Handle bounced\n";
  }
}

// Initializes a handle using a socket and passes it to context
static curl_context_t *create_curl_context(curl_socket_t sockfd)
{
  curl_context_t *context;

  context = (curl_context_t *)malloc(sizeof(*context));

  context->sockfd = sockfd;

  uv_poll_init_socket(loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;

  return context;
}

// Frees data from curl handle inside uv_handle*
static void curl_close_cb(uv_handle_t *handle)
{
  curl_context_t *context = (curl_context_t *)handle->data;
  free(context);
}

// Makes an asynchronious call to free curl context*
static void destroy_curl_context(curl_context_t *context)
{
  uv_close((uv_handle_t *)&context->poll_handle, curl_close_cb);
}

CurlHandleContext* createCurlHandleContext(CURL *handle, int index)
{
  auto context = new CurlHandleContext();
  context->curlHandle = handle;
  context->inUse = true;
  context->index = index;

  auto data = new UvTimerHandleData();
  // data->curlHandleContext = context;
  context->timerHandle.data = data;
  uv_timer_init(loop, &(context->timerHandle));

  return context;
}

// Curl function for writing data from call to memory
size_t myCallback(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  char* conts = (char*)contents;
  for(int i = 0; i < nmemb; i++) {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

// Adds easy handle that is ready to download data to multi handle 
static void startDownload(std::string *dst, std::string url)
{
  CURL *handle = nullptr;
  int *handleIndex = (int*)malloc(sizeof(int));

  // Search for unused handle
  for(int i = 0; i < handleVector.size(); i++) {
    if (!handleVector[i]->inUse) {
      handleVector[i]->inUse = true;

      // Refreshing timer
      UvTimerHandleData *data = (UvTimerHandleData*)(&(handleVector[i]->timerHandle.data));
      data->inUse = true;
      data->refresh = true;

      handle = handleVector[i]->curlHandle;
      *handleIndex = i;
      std::cout << "REUSING HANDLE\n";
      break;
    }
  }

  // In no unused handle found then create handle
  if (handle == nullptr) {
    handle = curl_easy_init();
    auto chc = createCurlHandleContext(handle, handleVector.size());

    handleVector.push_back(chc);
    //ind = (int*)malloc(sizeof(int));
    *handleIndex = handleVector.size()-1;
  }

  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, myCallback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  curl_easy_setopt(handle, CURLOPT_PRIVATE, handleIndex); // Memory is lost when overwriting previous int! TODO: FIX

  curl_multi_add_handle(curl_handle, handle);
}

// Removes used easy handles from multihandle
static void check_multi_info(void)
{
  char *done_url;
  CURLMsg *message;
  int pending;
  CURL *easy_handle;

  while ((message = curl_multi_info_read(curl_handle, &pending)))
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
      printf("%s DONE\n", done_url);

      int *ind;
      curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &ind);

      // Mark handle as not in use
      handleVector[*ind]->inUse = false;

      // Start timer for removal
      uv_timer_t *timerHandle = &(handleVector[*ind]->timerHandle);
      UvTimerHandleData *data = (UvTimerHandleData*)timerHandle->data;
      data->refresh = true;
      data->inUse = false;
      uv_timer_start(timerHandle, timerCallback, curlBuffer, curlBuffer);

      curl_multi_remove_handle(curl_handle, easy_handle);
      // curl_easy_cleanup(easy_handle);
    }
      break;

    default:
      fprintf(stderr, "CURLMSG default\n");
      break;
    }
  }
}

// Is used to react to polling file descriptors in poll_handle
// Calls handle_socket indirectly for further reading*
// If call is finished closes handle indirectly by check multi info
static void curl_perform(uv_poll_t *req, int status, int events)
{
  int running_handles;
  int flags = 0;
  curl_context_t *context;

  if (events & UV_READABLE)
    flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE)
    flags |= CURL_CSELECT_OUT;

  context = (curl_context_t *)req->data;

  curl_multi_socket_action(curl_handle, context->sockfd, flags,
                           &running_handles);

  check_multi_info();
}

static void on_timeout(uv_timer_t *req)
{
  int running_handles;
  curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0,
                           &running_handles);
  check_multi_info();
}

// Connects curl timer with uv timer
static int start_timeout(CURLM *multi, long timeout_ms, void *userp)
{
  if (timeout_ms < 0)
  {
    uv_timer_stop(&timeout);
  }
  else
  {
    if (timeout_ms == 0)
      timeout_ms = 1; /* 0 means directly call socket_action, but we will do it
                         in a bit */
    uv_timer_start(&timeout, on_timeout, timeout_ms, 0);
  }
  return 0;
}

// Is used to react to curl_multi_socket_action 
// If INOUT then assigns socket to multi handle and starts polling file descriptors in poll_handle by callback
static int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp,
                         void *socketp)
{
  curl_context_t *curl_context;
  int events = 0;

  switch (action)
  {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    curl_context = socketp ? (curl_context_t *)socketp : create_curl_context(s);

    curl_multi_assign(curl_handle, s, (void *)curl_context);

    if (action != CURL_POLL_IN)
      events |= UV_WRITABLE;
    if (action != CURL_POLL_OUT)
      events |= UV_READABLE;

    uv_poll_start(&curl_context->poll_handle, events, curl_perform);
    break;
  case CURL_POLL_REMOVE:
    if (socketp)
    {
      uv_poll_stop(&((curl_context_t *)socketp)->poll_handle);
      destroy_curl_context((curl_context_t *)socketp);
      curl_multi_assign(curl_handle, s, NULL);
    }
    break;
  default:
    abort();
  }

  return 0;
}

// -------------------------------------------------------------------------------------------------------------------

// Downloads contents from urls to contentMap in parallel
void runDownloadsFromMap(std::unordered_map<std::string, std::string*> *urlContentMap)
{
  for (std::pair<std::string, std::string*> urlContentPair : *urlContentMap)
  {
    std::string url = urlContentPair.first;
    startDownload(urlContentPair.second, url);
  }
}

std::unordered_map<std::string, std::string*> *urlVectorToUrlContentMap(std::vector<std::string> urlVector)
{
  auto urlContentMap = new std::unordered_map<std::string, std::string*>();
  for(int i = 0; i < urlVector.size(); i++) {
    (*urlContentMap)[urlVector[i]] = new std::string();
  }
  return urlContentMap;
}

int tries = 10000;
void checkDownloadTasks(uv_prepare_t *handle)
{
  if (tries-- == 0)
  {
    uv_prepare_stop(handle);
    std::cout << "STOPPED POLLING\n";
  }

  for(int i = 0; i < queueStatus.size(); i++)
  {
    if (queueStatus[i] == 0)
    {
      runDownloadsFromMap(urlContentMapQueue[i]);
      queueStatus[i] = 1;
    }
  }
}

void printBar()
{
  std::cout << "------------------------------------\n";
}

void printContents(std::unordered_map<std::string, std::string*> *urlContentMap)
{
  std::cout << "\n";
  for (std::pair<std::string, std::string*> element : *urlContentMap)
  {
    printBar();
    std::cout << element.first << "\n\n"
              << (element.second)->substr(0, 1000) << std::endl;
  }
}

int oldMain()
{
  auto start = std::chrono::steady_clock::now();

  loop = uv_default_loop();

  // Testing handle that runs every loop
  // uv_prepare_t *handle = (uv_prepare_t*)malloc(sizeof(uv_prepare_t)); // handle that runs once every loop
  uv_prepare_t timerHandle;  // handle that runs once every loop
  uv_prepare_init(loop, &timerHandle);
  uv_prepare_start(&timerHandle, checkDownloadTasks);

  // Testing timer handle
  // uv_timer_t timerHandle;
  // uv_timer_init(loop, &timerHandle);
  // uv_timer_start(&timerHandle, timerCallback, curlBuffer, 0);


  // Preparing curl and timeout
  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }
  uv_timer_init(loop, &timeout);

  curl_handle = curl_multi_init();
  curl_multi_setopt(curl_handle, CURLMOPT_SOCKETFUNCTION, handle_socket);
  curl_multi_setopt(curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout);

  uv_run(loop, UV_RUN_DEFAULT);


  // Cleaning up curl
  for(int i = 0; i < handleVector.size(); i++) {
    curl_easy_cleanup(handleVector[i]->curlHandle);    
  }
  curl_multi_cleanup(curl_handle);

  
  // Showing execution time
  // auto end = std::chrono::steady_clock::now();
  // printBar();
  // std::cout << "Elapsed time in microseconds: "
  //       << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
  //       << " µs" << std::endl;

  // // Cleaning UV loop
  // std::cout << "\nIs loop alive? " << uv_loop_alive(loop) << "\n";
  // uv_stop(loop);
  // std::cout << "Is loop alive? " << uv_loop_alive(loop) << "\n";
  // std::cout << "Loop closed properly? " << (uv_loop_close(loop) != UV_EBUSY)  << "\n";
  // free(loop);

  return 0;
}

int addDownloadTask(std::vector<std::string> urlVector)
{
  auto urlContentMap = urlVectorToUrlContentMap(urlVector);
  // mutex?
  urlContentMapQueue.push_back(urlContentMap);
  queueStatus.push_back(0);
  // end mutex?

  // 0 - queued
  // 1 - started
  // 2 - finished

  // returns index in urlContentMapQueue
  return queueStatus.size()-1;
}

std::unordered_map<std::string, std::string*> *getResponse(int index)
{
  return urlContentMapQueue[index];
}

std::string* getResponse(int index, std::string url)
{
  if (getResponse(index)->find(url) == getResponse(index)->end() )
  {
    std::cout << "Response to url: " << url << " NOT FOUND\n";
  }
  return (*getResponse(index))[url];
}

int main()
{
  std::thread t1(oldMain);

  std::vector<std::string> urlVec;
  urlVec.push_back("http://ccdb-test.cern.ch:8080/browse/TPC/.*");
  urlVec.push_back("http://ccdb-test.cern.ch:8080/latest/TPC/.*");
  int firstResponse = addDownloadTask(urlVec);

  sleep(2);
  std::cout << "Pushing second!\n";
  std::vector<std::string> urlVec2;
  urlVec2.push_back("http://alice-ccdb.cern.ch/browse/TPC/.*");
  urlVec2.push_back("http://alice-ccdb.cern.ch/latest/TPC/.*");
  int secondResponse = addDownloadTask(urlVec2);

  t1.join();

  // printContents(getResponse(0));
  // printContents(getResponse(1));
  // std::cout << "Response:\n" << *getResponse(0, "http://ccdb-test.cern.ch:8080/latest/TPC/.*");

  return 0;
}