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

- reusing socket errors can happen - handle by trying new socket, and pass error as last resort

- Maybe better interface for requesting downloads?
- multiple uv loop threads

Questions:

- are url's for requests unique?

*/


bool twoBatches = true;

class AsynchronousDownloader {

public:

uint64_t curlBuffer = 950; // miliseconds durning which handle will be left open after last call
int loopIterations = 5000;

bool closeLoop = false;
uv_loop_t *loop;
CURLM *curl_handle;
uv_timer_t timeout;

typedef struct curl_context_s
{
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
} curl_context_t;

typedef struct CurlHandleData
{
  bool inUse = true;
  int index;
  CURL* curlHandle;
  uv_timer_t timerHandle;
  int queueIndex;
} CurlHandleData;

typedef struct UvTimerHandleData
{
  bool refresh = true;
  CurlHandleData *curlHandleData = nullptr;
} UvTimerHandleData;


std::vector<std::unordered_map<std::string, std::string*>*> urlContentMapQueue;
std::vector<int> queueStatus;
std::vector<int> queueProgress;
std::unordered_map<int, CurlHandleData*> handleDataMap;

void AsynchronousDownloader::timerCallback(uv_timer_t *handle)
{
  auto timerData = (UvTimerHandleData*)(handle->data);

  if (!timerData->curlHandleData->inUse) {
    if (timerData->refresh) {
      timerData->refresh = false;
      // std::cout << "Handle about to expire\n";
    } else {
      uv_timer_stop(handle);
      // std::cout << "Handle expired\n";
      
      auto curlHandleData = timerData->curlHandleData;
      int index = curlHandleData->index;
      std::cout << "Handle expired at index: " << index << "\n";
      free(handleDataMap[index]);
      handleDataMap.erase(index);
      return;
    }
  }

  if (timerData->refresh) {
    uv_timer_again(handle);
    // std::cout << "Handle bounced\n";
  }
}

// Initializes a handle using a socket and passes it to context
curl_context_t* AsynchronousDownloader::create_curl_context(curl_socket_t sockfd)
{
  curl_context_t *context;

  context = (curl_context_t *)malloc(sizeof(*context));

  context->sockfd = sockfd;

  uv_poll_init_socket(loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;

  return context;
}

// Frees data from curl handle inside uv_handle*
static void AsynchronousDownloader::curl_close_cb(uv_handle_t *handle)
{
  curl_context_t *context = (curl_context_t *)handle->data;
  free(context);
}

// Makes an asynchronious call to free curl context*
static void destroy_curl_context(curl_context_t *context)
{
  uv_close((uv_handle_t *)&context->poll_handle, curl_close_cb);
}

// Obviously not safe for int overflow. Needs fixing!
int AsynchronousDownloader::createCurlHandleIndex()
{
  int i = 0;
  for (std::pair<int, CurlHandleData*> indexHandleDataPair : handleDataMap) {
    if (i <= indexHandleDataPair.first) i = indexHandleDataPair.first + 1;
  }
  return i;
}

CurlHandleData* AsynchronousDownloader::createCurlHandleData(CURL *handle, int queueIndex)
{
  auto context = (CurlHandleData*)malloc(sizeof(CurlHandleData));
  context->curlHandle = handle;
  context->inUse = true;
  context->index = createCurlHandleIndex();
  context->queueIndex = queueIndex;

  auto timerHandleData = (UvTimerHandleData*)malloc(sizeof(UvTimerHandleData));
  timerHandleData->curlHandleData = context;
  context->timerHandle.data = timerHandleData;

  uv_timer_init(loop, &(context->timerHandle));

  return context;
}

// Curl function for writing data from call to memory
size_t AsynchronousDownloader::myCallback(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  char* conts = (char*)contents;
  for(int i = 0; i < nmemb; i++) {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

// Adds easy handle that is ready to download data to multi handle 
void AsynchronousDownloader::startDownload(std::string *dst, std::string url, int queueInd)
{
  CURL *handle = nullptr;
  int *handleIndex = (int*)malloc(sizeof(int));

  // Search for unused handle
  for (std::pair<int, CurlHandleData*> indexHandleDataPair : handleDataMap) {
    auto handleData = indexHandleDataPair.second;    
    if (!handleData->inUse) {
      handleData->inUse = true;
      handleData->queueIndex = queueInd;
      // Setting timer to refresh
      auto data = (UvTimerHandleData *)(handleData->timerHandle.data);
      data->curlHandleData->inUse = true;
      data->refresh = true;

      handle = handleData->curlHandle;
      *handleIndex = indexHandleDataPair.first;
      std::cout << "Reusing handle at index: " << handleData->index << "\n";
      break;
    }
  }

  // In no unused handle found then create handle
  if (handle == nullptr) {
    handle = curl_easy_init();
    auto chc = createCurlHandleData(handle, queueInd);
    handleDataMap[chc->index] = chc;

    *handleIndex = chc->index;
    std::cout << "Creating handle at index: " << chc->index << "\n";
  }

  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, myCallback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  curl_easy_setopt(handle, CURLOPT_PRIVATE, handleIndex);

  curl_multi_add_handle(curl_handle, handle);
}

// Removes used easy handles from multihandle
void AsynchronousDownloader::check_multi_info(void)
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
      handleDataMap[*ind]->inUse = false;
      queueProgress[handleDataMap[*ind]->queueIndex]++;

      // Start timer for removal
      uv_timer_t *timerHandle = &(handleDataMap[*ind]->timerHandle);
      auto data = (UvTimerHandleData*)timerHandle->data;
      data->refresh = true;
      data->curlHandleData->inUse = false;
      uv_timer_start(timerHandle, timerCallback, curlBuffer, curlBuffer);


      free(ind);
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
void AsynchronousDownloader::curl_perform(uv_poll_t *req, int status, int events)
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

void AsynchronousDownloader::on_timeout(uv_timer_t *req)
{
  int running_handles;
  curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0,
                           &running_handles);
  check_multi_info();
}

// Connects curl timer with uv timer
int start_timeout(CURLM *multi, long timeout_ms, void *userp)
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
int AsynchronousDownloader::handle_socket(CURL *easy, curl_socket_t s, int action, void *userp,
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
void AsynchronousDownloader::runDownloadsFromMap(std::unordered_map<std::string, std::string*> *urlContentMap, int queueIndex)
{
  for (std::pair<std::string, std::string*> urlContentPair : *urlContentMap)
  {
    std::string url = urlContentPair.first;
    startDownload(urlContentPair.second, url, queueIndex);
  }
}

std::unordered_map<std::string, std::string*>* AsynchronousDownloader::urlVectorToUrlContentMap(std::vector<std::string> urlVector)
{
  auto urlContentMap = new std::unordered_map<std::string, std::string*>();
  for(int i = 0; i < urlVector.size(); i++) {
    (*urlContentMap)[urlVector[i]] = new std::string();
  }
  return urlContentMap;
}

void AsynchronousDownloader::checkDownloadTasks(uv_timer_t *handle)
{
  if (closeLoop)
  {
    uv_timer_stop(handle);
  }

  for(int i = 0; i < queueStatus.size(); i++)
  {
    if (queueStatus[i] == 0)
    {
      runDownloadsFromMap(urlContentMapQueue[i], i);
      queueStatus[i] = 1;
    }
  }
}

void AsynchronousDownloader::printBar()
{
  std::cout << "------------------------------------\n";
}

void AsynchronousDownloader::printContents(std::unordered_map<std::string, std::string*> *urlContentMap)
{
  std::cout << "\n";
  for (std::pair<std::string, std::string*> element : *urlContentMap)
  {
    printBar();
    std::cout << element.first << "\n\n"
              << (element.second)->substr(0, 1000) << std::endl;
  }
}

int AsynchronousDownloader::oldMain()
{
  auto start = std::chrono::steady_clock::now();

  loop = uv_default_loop();

  // Testing handle that runs every loop
  // uv_prepare_t *handle = (uv_prepare_t*)malloc(sizeof(uv_prepare_t)); // handle that runs once every loop
  // uv_prepare_t timerHandle;  // handle that runs once every loop
  // uv_prepare_init(loop, &timerHandle);
  // uv_prepare_start(&timerHandle, checkDownloadTasks);

  // Testing timer handle
  // uv_timer_t timerHandle;
  // uv_timer_init(loop, &timerHandle);
  // uv_timer_start(&timerHandle, timerCallback, curlBuffer, 0);

  uv_timer_t timerCheckQueueHandle;
  uv_timer_init(loop, &timerCheckQueueHandle);
  uv_timer_start(&timerCheckQueueHandle, checkDownloadTasks, 200, 200);


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
  for (std::pair<int, CurlHandleData*> indexHandleDataPair : handleDataMap) {
    curl_easy_cleanup(indexHandleDataPair.second->curlHandle);
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

int AsynchronousDownloader::addDownloadTask(std::vector<std::string> urlVector)
{
  auto urlContentMap = urlVectorToUrlContentMap(urlVector);
  // mutex?
  urlContentMapQueue.push_back(urlContentMap);
  queueStatus.push_back(0);
  queueProgress.push_back(0);
  // end mutex?

  // 0 - queued
  // 1 - started
  // 2 - finished

  // returns index in urlContentMapQueue
  return queueStatus.size()-1;
}

std::unordered_map<std::string, std::string*>* AsynchronousDownloader::getResponse(int index)
{
  return urlContentMapQueue[index];
}

std::string* AsynchronousDownloader::getResponse(int index, std::string url)
{
  if (getResponse(index)->find(url) == getResponse(index)->end() )
  {
    std::cout << "Response to url: " << url << " NOT FOUND\n";
  }
  return (*getResponse(index))[url];
}

};

int main()
{
  AsynchronousDownloader* AS = new AsynchronousDownloader();
  std::thread t1(AS->oldMain());

  std::vector<std::string> urlVec;
  urlVec.push_back("http://ccdb-test.cern.ch:8080/browse/TPC/.*");
  urlVec.push_back("http://ccdb-test.cern.ch:8080/latest/TPC/.*");
  int firstResponse = AS->addDownloadTask(urlVec);

  if (twoBatches) {
    sleep(2);
    std::cout << "Pushing second!\n";
    std::vector<std::string> urlVec2;
    urlVec2.push_back("http://alice-ccdb.cern.ch/browse/TPC/.*");
    urlVec2.push_back("http://alice-ccdb.cern.ch/latest/TPC/.*");
    int secondResponse = addDownloadTask(urlVec2);
  }

  while (queueProgress[0] != 2 || queueProgress[1] != 2) {
    sleep(1);
  }

  std::cout << "Signalled to close loop\n";
  closeLoop = true;
  t1.join();
  std::cout << "All worked well!\n";

  printContents(getResponse(0));
  printContents(getResponse(1));
  // std::cout << "Response:\n" << *getResponse(0, "http://ccdb-test.cern.ch:8080/latest/TPC/.*");

  return 0;
}