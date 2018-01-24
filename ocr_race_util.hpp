#ifndef __OCR_RACE_UTIL__
#define __OCR_RACE_UTIL__

#include <cstddef>
#include <string>



uint64_t generateTaskID();

uint64_t generateCacheKey(const uint64_t& id1, const uint64_t& id2);

/**
 * Test whether s2 is the suffix of s1
 */
bool isEndWith(std::string& s1, std::string& s2);


#endif
