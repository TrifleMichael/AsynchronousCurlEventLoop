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
    int secondResponse = AS->addDownloadTask(urlVec2);
  }

  while (AS->queueProgress[0] != 2 || AS->queueProgress[1] != 2) {
    sleep(1);
  }

  std::cout << "Signalled to close loop\n";
  AS->closeLoop = true;
  t1.join();
  std::cout << "All worked well!\n";

  AS->printContents(AS->getResponse(0));
  AS->printContents(AS->getResponse(1));
  // std::cout << "Response:\n" << *getResponse(0, "http://ccdb-test.cern.ch:8080/latest/TPC/.*");

  return 0;
}