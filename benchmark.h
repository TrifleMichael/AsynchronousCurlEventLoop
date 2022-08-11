#include <string>
#include <vector>
#include "rapidjson/rapidjson.h"
#include "rapidjson/document.h"

size_t writeToString2(void *contents, size_t size, size_t nmemb, std::string *dst);
std::string getStringAttribute(int ind, std::string attributeName, rapidjson::Document *doc);
std::string getLongAttribute(int ind, std::string attributeName, rapidjson::Document *doc);
size_t countObjects(rapidjson::Document *doc);
std::vector<std::string*> createPathsFromMetadata(std::string metadata, std::string baseUrl);
std::vector<std::string*> createPaths();








