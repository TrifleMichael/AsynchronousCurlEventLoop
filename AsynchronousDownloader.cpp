
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

#include <chrono>   // time measurement
#include <unistd.h> // time measurement

#include "AsynchronousDownloader.h"

/*
g++ -std=c++11 AsynchronousDownloader.cpp -lpthread -lcurl -luv -o main && ./main
*/

/*
TODO:

- reusing socket errors can happen - handle by trying new socket, and pass error as last resort
- interface for receiving and using handle from outside without changing it's settings

- Maybe better interface for requesting downloads?
- multiple uv loop threads

Questions:

Information:

- Curl multi handle automatically reuses connections. Source: https://everything.curl.dev/libcurl/connectionreuse

*/

// Handles unique timer that checks for downloads. Timer is unique because it stores different data than all other timer handles, which is only the object reference.
void checkDownloadTasks(uv_timer_t *handle)
{
  AsynchronousDownloader *AD = (AsynchronousDownloader *)handle->data;
  if (AD->closeLoop)
  {
    uv_timer_stop(handle);
  }

  for (int i = 0; i < AD->queueStatus.size(); i++)
  {
    if (AD->queueStatus[i] == 0)
    {
      AD->runDownloadsFromMap(AD->urlContentMapQueue[i], i);
      AD->queueStatus[i] = 1;
    }
  }
}

void timerCallbackForHandle(uv_timer_t *handle)
{
  auto timerData = (AsynchronousDownloader::UvTimerHandleData *)(handle->data);
  AsynchronousDownloader *AD = timerData->downloaderPtr;

  if (!timerData->curlHandleData->inUse)
  {
    if (timerData->refresh)
    {
      timerData->refresh = false;
    }
    else
    {
      uv_timer_stop(handle);
      auto curlHandleData = timerData->curlHandleData;
      int index = curlHandleData->index;
      std::cout << "Handle expired at index: " << index << "\n";
      free(AD->handleDataMap[index]);
      AD->handleDataMap.erase(index);
      return;
    }
  }

  if (timerData->refresh)
  {
    uv_timer_again(handle);
  }
}

void onTimeout(uv_timer_t *req)
{
  auto AD = (AsynchronousDownloader *)req->data;
  int running_handles;
  curl_multi_socket_action(AD->curlMultiHandle, CURL_SOCKET_TIMEOUT, 0,
                           &running_handles);
  AD->checkMultiInfo();
}

// Is used to react to polling file descriptors in poll_handle
// Calls handle_socket indirectly for further reading*
// If call is finished closes handle indirectly by check multi info
void curl_perform(uv_poll_t *req, int status, int events)
{
  int running_handles;
  int flags = 0;
  AsynchronousDownloader::curl_context_t *context;
  if (events & UV_READABLE)
    flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE)
    flags |= CURL_CSELECT_OUT;

  context = (AsynchronousDownloader::curl_context_t *)req->data;

  curl_multi_socket_action(context->objPtr->curlMultiHandle, context->sockfd, flags,
                           &running_handles);
  context->objPtr->checkMultiInfo();
}

// Initializes a handle using a socket and passes it to context
AsynchronousDownloader::curl_context_t *AsynchronousDownloader::createCurlContext(curl_socket_t sockfd, AsynchronousDownloader *objPtr)
{
  curl_context_t *context;

  context = (curl_context_t *)malloc(sizeof(*context));
  context->objPtr = objPtr;
  context->sockfd = sockfd;

  uv_poll_init_socket(objPtr->loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;

  return context;
}

// Frees data from curl handle inside uv_handle*
void AsynchronousDownloader::curlCloseCB(uv_handle_t *handle)
{
  curl_context_t *context = (curl_context_t *)handle->data;
  free(context);
}

// Makes an asynchronious call to free curl context*
void AsynchronousDownloader::destroyCurlContext(curl_context_t *context)
{
  uv_close((uv_handle_t *)&context->poll_handle, curlCloseCB);
}

// Obviously not safe for int overflow. Needs fixing!
int AsynchronousDownloader::createCurlHandleIndex(std::unordered_map<int, AsynchronousDownloader::CurlHandleData *> *handleDataMap)
{
  int i = 0;
  for (std::pair<int, CurlHandleData *> indexHandleDataPair : *handleDataMap)
  {
    if (i <= indexHandleDataPair.first)
      i = indexHandleDataPair.first + 1;
  }
  return i;
}

AsynchronousDownloader::CurlHandleData *AsynchronousDownloader::createCurlHandleData(CURL *handle, int queueIndex, std::unordered_map<int, AsynchronousDownloader::CurlHandleData *> *handleDataMap, uv_loop_t *loop)
{
  auto data = (CurlHandleData *)malloc(sizeof(CurlHandleData));
  data->curlHandle = handle;
  data->inUse = true;
  data->index = createCurlHandleIndex(handleDataMap);
  data->queueIndex = queueIndex;

  auto timerHandleData = (UvTimerHandleData *)malloc(sizeof(UvTimerHandleData));
  timerHandleData->curlHandleData = data;
  data->timerHandle.data = timerHandleData;

  uv_timer_init(loop, &(data->timerHandle));

  return data;
}

// Curl function for writing data from call to memory
size_t AsynchronousDownloader::curlCallback(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  char *conts = (char *)contents;
  for (int i = 0; i < nmemb; i++)
  {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

// Adds easy handle that is ready to download data to multi handle
void AsynchronousDownloader::startDownload(std::string *dst, std::string url, int queueInd, std::unordered_map<int, CurlHandleData *> *handleDataMap, uv_loop_t *loop, CURLM *curlMultiHandle)
{
  CURL *handle = nullptr;
  int *handleIndex = (int *)malloc(sizeof(int));

  // Search for unused handle
  for (std::pair<int, CurlHandleData *> indexHandleDataPair : *handleDataMap)
  {
    auto handleData = indexHandleDataPair.second;
    if (!handleData->inUse)
    {
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
  if (handle == nullptr)
  {
    handle = curl_easy_init();
    auto chc = createCurlHandleData(handle, queueInd, handleDataMap, loop);
    (*handleDataMap)[chc->index] = chc;

    *handleIndex = chc->index;
    std::cout << "Creating handle at index: " << chc->index << "\n";
  }

  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curlCallback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  curl_easy_setopt(handle, CURLOPT_PRIVATE, handleIndex);

  curl_multi_add_handle(curlMultiHandle, handle);
}

// Removes used easy handles from multihandle
void AsynchronousDownloader::checkMultiInfo(void)
{
  char *done_url;
  CURLMsg *message;
  int pending;
  CURL *easy_handle;

  while ((message = curl_multi_info_read(curlMultiHandle, &pending)))
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

      // int *ind;
      // curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &ind);

      // // Mark handle as not in use
      // handleDataMap[*ind]->inUse = false;
      // queueProgress[handleDataMap[*ind]->queueIndex]++;

      // Start timer for removal
      // uv_timer_t *timerHandle = &(handleDataMap[*ind]->timerHandle);
      // auto data = (UvTimerHandleData *)timerHandle->data;
      // data->refresh = true;
      // data->curlHandleData->inUse = false;
      // data->downloaderPtr = this;
      // uv_timer_start(timerHandle, timerCallbackForHandle, curlBuffer, curlBuffer);

      // free(ind);
      bool *completionFlag;
      curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &completionFlag);
      *completionFlag = true;      
      curl_multi_remove_handle(curlMultiHandle, easy_handle);
      // curl_easy_cleanup(easy_handle);
    }
    break;

    default:
      fprintf(stderr, "CURLMSG default\n");
      break;
    }
  }
}

// Connects curl timer with uv timer
int AsynchronousDownloader::startTimeout(CURLM *multi, long timeout_ms, void *userp)
{
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

// Is used to react to curl_multi_socket_action
// If INOUT then assigns socket to multi handle and starts polling file descriptors in poll_handle by callback
int AsynchronousDownloader::handleSocket(CURL *easy, curl_socket_t s, int action, void *userp,
                                         void *socketp)
{
  auto socketData = (DataForSocket *)userp;
  curl_context_t *curl_context;
  int events = 0;

  switch (action)
  {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    curl_context = socketp ? (curl_context_t *)socketp : createCurlContext(s, socketData->objPtr);
    curl_multi_assign(socketData->curlm, s, (void *)curl_context);

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
      destroyCurlContext((curl_context_t *)socketp);
      curl_multi_assign(socketData->curlm, s, NULL);
    }
    break;
  default:
    abort();
  }

  return 0;
}

// -------------------------------------------------------------------------------------------------------------------

// Downloads contents from urls to contentMap in parallel
void AsynchronousDownloader::runDownloadsFromMap(std::unordered_map<std::string, std::string *> *urlContentMap, int queueIndex)
{
  for (std::pair<std::string, std::string *> urlContentPair : *urlContentMap)
  {
    std::string url = urlContentPair.first;
    startDownload(urlContentPair.second, url, queueIndex, &handleDataMap, loop, curlMultiHandle);
  }
}

std::unordered_map<std::string, std::string *> *AsynchronousDownloader::urlVectorToUrlContentMap(std::vector<std::string> urlVector)
{
  auto urlContentMap = new std::unordered_map<std::string, std::string *>();
  for (int i = 0; i < urlVector.size(); i++)
  {
    (*urlContentMap)[urlVector[i]] = new std::string();
  }
  return urlContentMap;
}

void AsynchronousDownloader::printBar()
{
  std::cout << "------------------------------------\n";
}

void AsynchronousDownloader::printContents(std::unordered_map<std::string, std::string *> *urlContentMap)
{
  std::cout << "\n";
  for (std::pair<std::string, std::string *> element : *urlContentMap)
  {
    printBar();
    std::cout << element.first << "\n\n"
              << (element.second)->substr(0, 1000) << std::endl;
  }
}

int AsynchronousDownloader::oldMain()
{
  timeout = new uv_timer_t();
  timeout->data = this;
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
  // uv_timer_start(&timerHandle, timerCallbackForHandle, curlBuffer, 0);

  // Preparing curl and timeout

  uv_timer_init(loop, timeout);

  curlMultiHandle = curl_multi_init();
  curl_multi_setopt(curlMultiHandle, CURLMOPT_SOCKETFUNCTION, handleSocket);
  DataForSocket socketData;
  socketData.curlm = curlMultiHandle;
  socketData.objPtr = this;
  curl_multi_setopt(curlMultiHandle, CURLMOPT_SOCKETDATA, &socketData);
  curl_multi_setopt(curlMultiHandle, CURLMOPT_TIMERFUNCTION, startTimeout);
  curl_multi_setopt(curlMultiHandle, CURLMOPT_TIMERDATA, timeout);

  uv_timer_t timerCheckQueueHandle;
  timerCheckQueueHandle.data = this;
  uv_timer_init(loop, &timerCheckQueueHandle);
  uv_timer_start(&timerCheckQueueHandle, checkDownloadTasks, 200, 200);

  std::cout << "Running loop\n";
  uv_run(loop, UV_RUN_DEFAULT);
  std::cout << "Loop finished\n";

  // Cleaning up curl
  for (std::pair<int, CurlHandleData *> indexHandleDataPair : handleDataMap)
  {
    curl_easy_cleanup(indexHandleDataPair.second->curlHandle);
  }
  curl_multi_cleanup(curlMultiHandle);

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
  return queueStatus.size() - 1;
}

std::unordered_map<std::string, std::string *> *AsynchronousDownloader::getResponse(int index)
{
  return urlContentMapQueue[index];
}

std::string *AsynchronousDownloader::getResponse(int index, std::string url)
{
  if (getResponse(index)->find(url) == getResponse(index)->end())
  {
    std::cout << "Response to url: " << url << " NOT FOUND\n";
  }
  return (*getResponse(index))[url];
}

void AsynchronousDownloader::startAlternativeDownload(CURL* handle)
{

  // curl_easy_setopt(handle, CURLOPT_PRIVATE, handleIndex); // what data to store?
  std::cout << "Alternative download ran\n";
  curl_multi_add_handle(curlMultiHandle, handle);
}

void blockingPerform(CURL* handle, AsynchronousDownloader *AD)
{
  bool completionFlag = false;
  curl_easy_setopt(handle, CURLOPT_PRIVATE, &completionFlag);
  curl_multi_add_handle(AD->curlMultiHandle, handle);
  std::cout << "D\n";
  while (!completionFlag) sleep(1);
  std::cout << "E\n";
}

size_t testCallback(void *contents, size_t size, size_t nmemb, std::string *dst)
{
  std::cout << "Callback\n";
  char *conts = (char *)contents;
  for (int i = 0; i < nmemb; i++)
  {
    (*dst) += *(conts++);
  }
  return size * nmemb;
}

CURL* prepareTestHandle(std::string* dst)
{
  CURL* handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_URL, "http://alice-ccdb.cern.ch/latest/TPC/.*");
  // curl_easy_setopt(handle, CURLOPT_URL, "https://www.google.com/?rand=131");
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, testCallback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  return handle;
}

int main()
{  
  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }
  std::string dst;
  CURL* testHandle = prepareTestHandle(&dst);

  AsynchronousDownloader *AD = new AsynchronousDownloader();
  std::thread t1(&AsynchronousDownloader::oldMain, AD);
  sleep(1);
  std::cout << "About to blocking perform\n";
  blockingPerform(testHandle, AD);

  std::cout << "Signalling end\n";
  AD->closeLoop = true;
  t1.join();
  std::cout << "Final: " << dst << "\n";
  return 0;
}

// bool twoBatches = true;
// int main()
// {
//   AsynchronousDownloader *AS = new AsynchronousDownloader();
//   std::thread t1(&AsynchronousDownloader::oldMain, AS);

//   std::vector<std::string> urlVec;
//   urlVec.push_back("http://ccdb-test.cern.ch:8080/browse/TPC/.*");
//   urlVec.push_back("http://ccdb-test.cern.ch:8080/latest/TPC/.*");
//   int firstResponse = AS->addDownloadTask(urlVec);

//   if (twoBatches)
//   {
//     sleep(2);
//     std::cout << "Pushing second!\n";
//     std::vector<std::string> urlVec2;
//     urlVec2.push_back("http://alice-ccdb.cern.ch/browse/TPC/.*");
//     urlVec2.push_back("http://alice-ccdb.cern.ch/latest/TPC/.*");
//     int secondResponse = AS->addDownloadTask(urlVec2);
//   }

//   while (AS->queueProgress[0] != 2 || AS->queueProgress[1] != 2)
//   {
//     sleep(1);
//   }

//   std::cout << "Signalled to close loop\n";
//   AS->closeLoop = true;
//   t1.join();
//   std::cout << "All worked well!\n";

//   // AS->printContents(AS->getResponse(0));
//   // AS->printContents(AS->getResponse(1));
//   // std::cout << "Response:\n" << *getResponse(0, "http://ccdb-test.cern.ch:8080/latest/TPC/.*");

//   return 0;
// }