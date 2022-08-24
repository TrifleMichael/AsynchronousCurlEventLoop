
/*
g++ -std=c++11 multiPerformTest.cpp -lpthread -lcurl -luv -o multiPerformTest && ./multiPerformTest
*/

#include <stdio.h>
#include <string.h>

/* somewhat unix-specific */
#include <sys/time.h>
#include <unistd.h>

/* curl stuff */
#include <curl/curl.h>
#include <vector>
#include <string>
#include "resources.h"
#include <iostream>
#include <unistd.h> // time measurement
#include <chrono>

/*
 * Download a HTTP file and upload an FTP file simultaneously.
 */

#define HANDLECOUNT 2   /* Number of simultaneous transfers */
#define HTTP_HANDLE 0   /* Index for the HTTP transfer */
#define FTP_HANDLE 1    /* Index for the FTP transfer */

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


int64_t testPerform(int TEST_SIZE)
{
  CURLM *multi_handle = curl_multi_init();
  curl_multi_setopt(multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, 1);

  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  std::vector<std::string*> results;


  int still_running = 1; /* keep number of running handles */
  int i;

  auto start = std::chrono::system_clock::now();

  CURLMsg *msg; /* for picking up messages with the transfer status */
  int msgs_left; /* how many messages are left */

  /* Allocate one CURL handle per transfer */
  std::vector<CURL*> handles;
  for(int i = 0; i < TEST_SIZE; i++) {
    results.push_back(new std::string());
    handles.push_back(createHandle(*paths[i], results.back(), nullptr));

    std::string etagHeader = "If-None-Match: \"" + *etags[i] + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handles.back(), CURLOPT_HTTPHEADER, curlHeaders);

    curl_multi_add_handle(multi_handle, handles.back());
  }

  /* add the individual transfers */
  for(i = 0; i<TEST_SIZE; i++)
    curl_multi_add_handle(multi_handle, handles[i]);

  while(still_running) {
    CURLMcode mc = curl_multi_perform(multi_handle, &still_running);

    if(still_running)
      /* wait for activity, timeout or "nothing" */
      mc = curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);

    if(mc)
      break;
  }

  /* remove the transfers and cleanup the handles */
  for(i = 0; i<TEST_SIZE; i++) {
    long code;
    curl_easy_getinfo(handles[i], CURLINFO_HTTP_CODE, &code);
    if (code != 304) {
      std::cout << "Error. Http code: " << code << "\n";
    }
    curl_multi_remove_handle(multi_handle, handles[i]);
    curl_easy_cleanup(handles[i]);
  }

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Perform " << difference << "ms.\n";

  curl_multi_cleanup(multi_handle);
  return difference;
}

int64_t testReuse(int TEST_SIZE)
{
  auto paths = createPathsFromCS();
  auto etags = createEtagsFromCS();
  std::vector<std::string*> results;


  auto start = std::chrono::system_clock::now();


  /* Allocate one CURL handle per transfer */
  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString2);

  for(int i = 0; i < TEST_SIZE; i++) {
    curl_easy_setopt(handle, CURLOPT_URL, paths[i]->c_str());
    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());

    std::string etagHeader = "If-None-Match: \"" + *etags[i] + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);

    curl_easy_perform(handle);
    long code;
    curl_easy_getinfo(handle, CURLINFO_HTTP_CODE, &code);
    if (code != 304) {
      std::cout << "Error. Http code: " << code << "\n";
    }

  }

  curl_easy_cleanup(handle);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Reuse " << difference << "ms.\n";
  return difference;
}

int main()
{
  int REAPEATS = 100;
  
  int performSum = 0;
  int reuseSum = 0;
  for(int i = 0; i < REAPEATS; i++) performSum += testPerform(495);
  for(int i = 0; i < REAPEATS; i++) reuseSum += testReuse(495);
  
  std::cout << "Reuse " << reuseSum / REAPEATS << " Perform " << performSum / REAPEATS << " Theoretical reuse " << reuseSum / REAPEATS * 1.5 << "\n";

  return 0;
}