#include <iostream>
#include "AsynchronousDownloader.h"
#include <curl/curl.h>
#include "resources.h"

/*
g++ -std=c++11 TestAD.cpp AsynchronousDownloader.cpp benchmark.cpp -lpthread -lcurl -luv -o TestAD && ./TestAD
*/

void etagTest()
{
  std::string dst;
  std::string headers;
  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &headers);

  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &dst);
  curl_easy_setopt(handle, CURLOPT_URL, "http://ccdb-test.cern.ch:8080/MFT/Config/AlpideParam/1");
  auto res = curl_easy_perform(handle);
  curl_easy_cleanup(handle);

  if (res != CURLE_OK) {
    std::cout << "Oops\n";
  } else {
    std::cout << dst << "\n";
    std::cout << "-------------------------------------\n";
    std::cout << headers << "\n";
    std::cout << "ETAG: " << extractETAG(headers) << "\n";
  }

  std::cout << "\nSECOND TRY\n\n";

  CURL *handle2 = curl_easy_init();
  std::string dst2;
  curl_easy_setopt(handle2, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle2, CURLOPT_WRITEDATA, &dst2);
  curl_easy_setopt(handle2, CURLOPT_URL, "http://ccdb-test.cern.ch:8080/MFT/Config/AlpideParam/1");

  struct curl_slist *curlHeaders=NULL;
  std::string etagHeader = "If-None-Match: \"" + extractETAG(headers) + "\"";
  std::cout << "etagHeader: " << etagHeader << "\n";
  curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
  curl_easy_setopt(handle2, CURLOPT_HTTPHEADER, curlHeaders);

  auto res2 = curl_easy_perform(handle2);
  long ret;
  curl_easy_getinfo(handle2, CURLINFO_RESPONSE_CODE, &ret);
  curl_easy_cleanup(handle2);
  if (res2 != CURLE_OK) {
    std::cout << "It didn't work. Code: " << res2 << "\n";
  } else {
    std::cout << "DST2" << dst2 << "\n";
    std::cout << "Ret" << ret << "\n";
  }
}

std::vector<std::string> createPathsFromCS()
{
  std::vector<std::string> vec;
  std::string temp = "";
  for(int i = 0; i < pathsCS.size(); i++)
  {
    if (pathsCS[i] == ',') {
      vec.push_back(temp);
      temp = "";
    }
    else {
      (temp.push_back(pathsCS[i]));
    }
  }
  return vec;
}

void blockingBatchTest(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  
  AsynchronousDownloader AD;
  AD.init();
  std::thread t(&AsynchronousDownloader::asynchLoop, &AD);

  auto start = std::chrono::system_clock::now();

  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL* handle = curl_easy_init();
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    handles.push_back(handle);
  }
  AD.batchBlockingPerform(handles);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "BLOCKING BATCH TEST - execution time: " << difference << "ms.\n";
  AD.closeLoop = true;
  t.join();
}

void asynchBatchTest(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  

  AsynchronousDownloader AD;
  AD.init();
  std::thread t(&AsynchronousDownloader::asynchLoop, &AD);

  auto start = std::chrono::system_clock::now();

  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL* handle = curl_easy_init();
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results.back());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    handles.push_back(handle);
  }

  bool requestFinished = false;
  AD.batchAsynchPerform(handles, &requestFinished);

  while (!requestFinished) sleep(0.05);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "ASYNCH BATCH TEST - execution time: " << difference << "ms.\n";
  AD.closeLoop = true;
  t.join();
}

void linearTest(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;

  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  auto start = std::chrono::system_clock::now();

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results[i]);
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());
    curl_easy_perform(handle);
    auto ret = curl_easy_perform(handle);
    if (ret != CURLE_OK) {
      std::cout << "Error in linearTest, code: " << ret << "\n";
    }
  }
  curl_easy_cleanup(handle);
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "LINEAR TEST - execution time: " << difference << "ms.\n";
}

void linearTestNoReuse(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;

  auto start = std::chrono::system_clock::now();

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL *handle = curl_easy_init();
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    results.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results[i]);
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());
    auto ret = curl_easy_perform(handle);
    curl_easy_cleanup(handle);
    if (ret != CURLE_OK) {
      std::cout << "Error in linearTestNoReuse, code: " << ret << "\n";
    }
  }
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "LINEAR NO REUSE TEST - execution time: " << difference << "ms.\n";
}

int main()
{
  if (curl_global_init(CURL_GLOBAL_ALL))
  {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }

  int testSize = 20;
  blockingBatchTest(testSize);
  asynchBatchTest(testSize);
  linearTest(testSize);
  linearTestNoReuse(testSize);

  curl_global_cleanup();

  return 0;
}