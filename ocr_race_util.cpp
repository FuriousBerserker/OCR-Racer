#include "atomic/ops-enum.hpp"
#include "atomic/ops.hpp"
#include "const_val.hpp"
#include "ocr_race_util.hpp"

uint32_t nextTaskID = 1;

uint64_t generateTaskID() {
    bool success = false;
    uint32_t nextID;
    do {
        nextID = ATOMIC::OPS::Load(&nextTaskID, ATOMIC::BARRIER_LD_NEXT);
        success =
            ATOMIC::OPS::CompareAndDidSwap(&nextTaskID, nextID, nextID + 1);
    } while (!success);
    return (uint64_t)nextID;
}

uint64_t generateCacheKey(const uint64_t& id1, const uint64_t& id2) {
    return (id1 << 32) | id2;
}

/**
 * Test whether s2 is the suffix of s1
 */
bool isEndWith(std::string& s1, std::string& s2) {
    if (s1.size() < s2.size()) {
        return false;
    } else {
        return !s1.compare(s1.size() - s2.size(), std::string::npos, s2);
    }
}
