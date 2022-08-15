#include <iostream>
#include "AsynchronousDownloader.h"
#include <curl/curl.h>
#include "resources.h"
#include <unordered_map>

/*
g++ -std=c++11 TestAD.cpp AsynchronousDownloader.cpp benchmark.cpp -lpthread -lcurl -luv -o TestAD && ./TestAD
*/

void setHandleOptions(CURL* handle, std::string* dst, std::string* headers, std::string* path)
{
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, headers);

  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, dst);
  curl_easy_setopt(handle, CURLOPT_URL, path->c_str());
  // curl_easy_perform(handle);
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

void blockingBatchTest(int pathLimit = 0)
{
  // Preparing for downloading
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;
  
  AsynchronousDownloader AD;
  AD.init();
  std::thread t(&AsynchronousDownloader::asynchLoop, &AD);

  auto start = std::chrono::system_clock::now();

  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    handles.push_back(curl_easy_init());
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handles.back(), results.back(), headers.back(), &paths[i]);
  }

  // Downloading objects
  AD.batchBlockingPerform(handles);
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "BLOCKING BATCH TEST - execution time: " << difference << "ms.\n";

  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  std::vector<CURL*> handles2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    auto handle = curl_easy_init();
    results2.push_back(new std::string());
    handles2.push_back(handle);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + extractETAG(urlETagMap[paths[i]]) + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
  }

  // Checking objects validity
  AD.batchBlockingPerform(handles2);
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    long code;
    curl_easy_getinfo(handles2[i], CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
  }

  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "BLOCKING BATCH TEST RECHECKING - execution time: " << difference2 << "ms.\n";

  // Cleaning up
  AD.closeLoop = true;
  t.join();
}

void asynchBatchTest(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;
  
  AsynchronousDownloader AD;
  AD.init();
  std::thread t(&AsynchronousDownloader::asynchLoop, &AD);

  auto start = std::chrono::system_clock::now();

  std::vector<CURL*> handles;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    handles.push_back(curl_easy_init());
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handles.back(), results.back(), headers.back(), &paths[i]);
  }

  bool requestFinished = false;
  AD.batchAsynchPerform(handles, &requestFinished);

  while (!requestFinished) sleep(0.05);

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "ASYNCH BATCH TEST - execution time: " << difference << "ms.\n";


  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  std::vector<CURL*> handles2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    auto handle = curl_easy_init();
    results2.push_back(new std::string());
    handles2.push_back(handle);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + extractETAG(urlETagMap[paths[i]]) + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
  }

  // Checking objects validity
  bool requestFinished2 = false;
  AD.batchAsynchPerform(handles2, &requestFinished2);
  while (!requestFinished2) sleep(0.001);

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    long code;
    curl_easy_getinfo(handles2[i], CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
  }

  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "ASYNC BATCH TEST RECHECKING - execution time: " << difference2 << "ms.\n";

  // Cleaning up
  AD.closeLoop = true;
  t.join();
}

void linearTest(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;

  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  auto start = std::chrono::system_clock::now();

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handle, results.back(), headers.back(), &paths[i]);
    curl_easy_perform(handle);
  }

  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "LINEAR TEST - execution time: " << difference << "ms.\n";

  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    results2.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + extractETAG(urlETagMap[paths[i]]) + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
    
    curl_easy_perform(handle);
    long code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
  }

  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "LINEAR TEST RECHECKING - execution time: " << difference2 << "ms.\n";

}

void linearTestNoReuse(int pathLimit = 0)
{
  auto paths = createPathsFromCS();
  std::vector<std::string*> results;
  std::vector<std::string*> headers;
  std::unordered_map<std::string, std::string> urlETagMap;

  auto start = std::chrono::system_clock::now();

  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL* handle = curl_easy_init();
    results.push_back(new std::string());
    headers.push_back(new std::string());
    setHandleOptions(handle, results.back(), headers.back(), &paths[i]);
    curl_easy_perform(handle);
    curl_easy_cleanup(handle);

  }
  auto end = std::chrono::system_clock::now();
  auto difference = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "LINEAR NO REUSE TEST - execution time: " << difference << "ms.\n";

  // Extracting etags
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    urlETagMap[paths[i]] = extractETAG(*headers[i]);
  }

  // Preparing for checking objects validity
  auto start2 = std::chrono::system_clock::now();

  std::vector<std::string*> results2;
  for (int i = 0; i < (pathLimit == 0 ? paths.size() : pathLimit); i++) {
    CURL* handle = curl_easy_init();
    results2.push_back(new std::string());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, results2.back());
    curl_easy_setopt(handle, CURLOPT_URL, paths[i].c_str());

    std::string etagHeader = "If-None-Match: \"" + extractETAG(urlETagMap[paths[i]]) + "\"";
    struct curl_slist *curlHeaders = nullptr;
    curlHeaders = curl_slist_append(curlHeaders, etagHeader.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);
    
    curl_easy_perform(handle);
    long code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    if (code != 304) {
      std::cout << "INVALID CODE: " << code << "\n";
    }
    curl_easy_cleanup(handle);
  }

  auto end2 = std::chrono::system_clock::now();
  auto difference2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
  std::cout << "LINEAR NO REUSE TEST RECHECKING - execution time: " << difference2 << "ms.\n";

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