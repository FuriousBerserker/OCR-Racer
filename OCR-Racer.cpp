
/*! @file
 *  This is an example of the PIN tool that demonstrates some basic PIN APIs
 *  and could serve as the starting point for developing your first PIN tool
 */

#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include "pin.H"
#define __USE_PIN_CAS
#include "ConcurrentHashMap.hpp"
#include "atomic/ops-enum.hpp"
#include "atomic/ops.hpp"
#include "ocr-types.h"

//#define DEBUG
#define GRAPH_CONSTRUCTION
#define INSTRUMENT
#define DETECT_RACE
#define MEASURE_TIME
//#define STATISTICS
//#define OUTPUT_CG
//#define OUTPUT_SOURCE


class Node;
class Task;
class Event;
class ComputationMap;

#ifdef MEASURE_TIME
struct timeval program_start, program_end;
#endif

const uint32_t NULL_ID = 0;

const uint32_t START_EPOCH = 0;
const uint32_t END_EPOCH = 0xFFFFFFFF;

uint32_t nextTaskID = 1;

// guid ---> task ID map
ConcurrentHashMap<uint64_t, uint64_t, NULL_ID, IdentityHash<uint64_t> > guidMap(
    10000);

// skipped library list
vector<std::string> skippedLibraries;

// user code image name
std::string userCodeImg;

// key for accessing TLS storage in the threads. initialized once in main()
static TLS_KEY tls_key = INVALID_TLS_KEY;

// output stream
std::ostream* out = &cout;
std::ostream* err = &cerr;
// std::ofstream logFile, errorFile;
// std::ostream *out = &logFile;
// std::ostream *err = &errorFile;
std::stringstream ss;

////////////////////////////////////////////////////////////////////////////////
//                            Utility Function                               //
///////////////////////////////////////////////////////////////////////////////

inline uint64_t generateTaskID() {
    bool success = false;
    uint32_t nextID;
    do {
        nextID = ATOMIC::OPS::Load(&nextTaskID, ATOMIC::BARRIER_LD_NEXT);
        success =
            ATOMIC::OPS::CompareAndDidSwap(&nextTaskID, nextID, nextID + 1);
    } while (!success);
    return (uint64_t)nextID;
}

inline uint64_t generateCacheKey(const uint64_t& id1, const uint64_t& id2) {
    return (id1 << 32) | id2;
}

/**
 * Test whether s2 is the suffix of s1
 */
inline bool isEndWith(std::string& s1, std::string& s2) {
    if (s1.size() < s2.size()) {
        return false;
    } else {
        return !s1.compare(s1.size() - s2.size(), std::string::npos, s2);
    }
}

/**
 * Test whether s2 is the prefix of s1
 */
inline bool isStartWith(std::string& s1, std::string& s2) {
    if (s1.size() < s2.size()) {
        return false;
    } else {
        return !s1.compare(0, s2.size(), s2);
    }
}

inline bool isOCRLibrary(IMG& img) {
    std::string ocrLibraryName = "libocr_x86.so";
    std::string imageName = IMG_Name(img);
    if (isEndWith(imageName, ocrLibraryName)) {
        return true;
    } else {
        return false;
    }
}

inline bool isOCRCall(RTN& rtn) {
    std::string ocrCallPrefix = "ocr";
    std::string rtnName = RTN_Name(rtn);
    if (isStartWith(rtnName, ocrCallPrefix)) {
        return true;
    } else {
        return false;
    }
}

bool isUserCodeImg(IMG& img) {
    std::string imageName = IMG_Name(img);
    if (imageName == userCodeImg) {
        return true;
    } else {
        return false;
    }
}

bool isIgnorableIns(INS& ins) {
    if (INS_IsStackRead(ins) || INS_IsStackWrite(ins)) return true;

    // skip call, ret and JMP instructions
    if (INS_IsBranchOrCall(ins) || INS_IsRet(ins)) {
        return true;
    }

    return false;
}

/**
 * Test whether guid id NULL_GUID
 */
bool isNullGuid(ocrGuid_t& guid) {
    if (guid.guid == NULL_GUID.guid) {
        return true;
    } else {
        return false;
    }
}

std::string getSourceInfo(ADDRINT ip) {
    int32_t column, line;
    std::string file;
    PIN_LockClient();
    PIN_GetSourceLocation(ip, &column, &line, &file);
    PIN_UnlockClient();
    ss.str("");
    ss << file << " " << line << " " << column;
    return ss.str();
}

    ////////////////////////////////////////////////////////////////////////////////
    //                            Computation Graph //
    ///////////////////////////////////////////////////////////////////////////////

#ifdef OUTPUT_CG
class ColorScheme {
   private:
    std::string color;
    std::string style;

   public:
    ColorScheme() {}
    ColorScheme(std::string color, std::string style)
        : color(color), style(style) {}
    virtual ~ColorScheme() {}
    std::string toString() {
        return "[color=" + color + ", style=" + style + "]";
    }
};
#endif

struct Dep {
   public:
    Node* src;
    uint32_t epoch;
    Dep(Node* src, uint32_t epoch = END_EPOCH) : src(src), epoch(epoch) {}
    Dep(const Dep& other) : Dep(other.src, other.epoch) {}
    virtual ~Dep() {}

    // Dep& operator=(const Dep& other) {
    // this->src = other.src;
    // this->epoch = other.epoch;
    // return *this;
    //}
    //
    // friend bool operator==(const Dep& a, const Dep& b) {
    // return a.src == b.src && a.epoch == b.epoch;
    //}
    //
    // friend bool operator!=(const Dep& a, const Dep& b) {
    // return !(a == b);
    //}
};

class Node {
   public:
    enum Type { TASK, DB, EVENT };
    enum EdgeType { SPAWN, JOIN, CONTINUE };

   protected:
    ConcurrentHashMap<uint64_t, Dep*, nullptr, IdentityHash<uint64_t> >
        incomingEdges;
    Task* parent;
    uint32_t parentEpoch;
    Type type;

   public:
    uint64_t id;
    uint64_t depth;

   public:
    Node(uint64_t id, Type type, Task* parent, uint32_t parentEpoch);
    virtual ~Node();
    void addDeps(uint64_t id, Dep& dep);
    void calculateDepth();

    friend class ComputationGraph;
};

class Task : public Node {
   public:
    Task(uint64_t id, Task* parent, uint32_t parentEpoch);
    virtual ~Task();
};

// class DataBlock : public Node {
// public:
// u16 accessMode;
// DBNode(intptr_t id, u16 accessMode);
// virtual ~DBNode();
//};
//

class Event : public Node {
   public:
    Event(uint64_t id);
    virtual ~Event();
};

class ComputationGraph {
   private:
    ConcurrentHashMap<uint64_t, Node*, nullptr, IdentityHash<uint64_t> >
        nodeMap;
    ConcurrentHashMap<uint64_t, uint32_t, START_EPOCH, IdentityHash<uint64_t> >
        cacheMap;
    ConcurrentHashMap<uint64_t, uint32_t, START_EPOCH, IdentityHash<uint64_t> >
        epochMap;

   public:
    ComputationGraph(uint64_t capacity);
    virtual ~ComputationGraph();
    void insert(uint64_t key, Node* value);
    Node* getNode(uint64_t key);
    bool isReachable(uint64_t srcID, uint32_t srcEpoch, uint64_t dstID,
                     uint32_t dstEpoch);
    bool isReachable(std::unordered_map<uint64_t, uint32_t>& srcNodes,
                     uint64_t dstID, uint32_t dstEpoch);
    void updateCache(uint64_t srcID, uint32_t srcEpoch, uint64_t dstID);
    void updateTaskFinalEpoch(uint64_t taskID, uint32_t epoch);
#ifdef OUTPUT_CG
    void toDot(std::ostream& out);
#endif

#ifdef STATISTICS
    void showStatistics(std::ostream& out);
#endif
   private:
#ifdef OUTPUT_CG
    std::map<Node::Type, ColorScheme> nodeColorSchemes;
    std::map<Node::EdgeType, ColorScheme> edgeColorSchemes;
    void outputLink(std::ostream& out, Node* n1, uint32_t epoch1, Node* n2,
                    uint32_t epoch2, Node::EdgeType edgeType);
#endif
};

Node::Node(uint64_t id, Type type, Task* parent, uint32_t parentEpoch)
    : incomingEdges(10),
      parent(parent),
      parentEpoch(parentEpoch),
      type(type),
      id(id),
      depth(0) {}

Node::~Node() {}

inline void Node::addDeps(uint64_t id, Dep& dep) {
    Dep* copy = new Dep(dep);
    incomingEdges.put(id, copy);
    //*out << id << "#" << dep.epoch << "->" << this->id << std::endl;
}

inline void Node::calculateDepth() {
    for (auto ei = incomingEdges.begin(), ee = incomingEdges.end(); ei != ee; ++ei) {
        Node* prev = (*ei).second->src;
        if (prev->depth == 0) {
            prev->calculateDepth();
        }
        if (depth < prev->depth) {
            depth = prev->depth;
        }
    }

    if (parent && depth < parent->depth) {
        depth = parent->depth;
    }
    depth += 1;
}

Task::Task(uint64_t id, Task* parent, uint32_t parentEpoch)
    : Node(id, Node::TASK, parent, parentEpoch) {}

Task::~Task() {}

Event::Event(uint64_t id) : Node(id, Node::EVENT, nullptr, END_EPOCH) {}

Event::~Event() {}

ComputationGraph::ComputationGraph(uint64_t hashUpperBound)
    : nodeMap(hashUpperBound),
      cacheMap(hashUpperBound),
      epochMap(hashUpperBound) {
#ifdef OUTPUT_CG
    ColorScheme a("green", "filled"), b("yellow", "filled"),
        c("blue", "filled");
    nodeColorSchemes.insert(std::make_pair(Node::TASK, a));
    nodeColorSchemes.insert(std::make_pair(Node::DB, b));
    nodeColorSchemes.insert(std::make_pair(Node::EVENT, c));
    ColorScheme e("red", "bold"), f("cyan", "bold"), g("black", "bold");
    edgeColorSchemes.insert(std::make_pair(Node::SPAWN, e));
    edgeColorSchemes.insert(std::make_pair(Node::JOIN, f));
    edgeColorSchemes.insert(std::make_pair(Node::CONTINUE, g));
#endif
}

ComputationGraph::~ComputationGraph() {}

inline void ComputationGraph::insert(uint64_t key, Node* value) {
    nodeMap.put(key, value);
}

inline Node* ComputationGraph::getNode(uint64_t key) {
    return nodeMap.get(key);
}

// A BFS search to check the reachability between nodes
bool ComputationGraph::isReachable(uint64_t srcID, uint32_t srcEpoch,
                                   uint64_t dstID, uint32_t dstEpoch) {
    if (srcID == dstID) {
        return true;
    }
    Node* dstNode = nodeMap.get(dstID);
    Node* srcNode = nodeMap.get(srcID);
    bool result = false;
    std::queue<Node*> q;
    std::set<uint64_t> accessedNodes;
    q.push(dstNode);
    accessedNodes.insert(dstID);
    while (!q.empty()) {
        Node* next = q.front();
        q.pop();
        // cout << "current: " << next->id << endl;
        uint64_t cacheKey = generateCacheKey(srcID, next->id);
        uint32_t cacheEpoch = cacheMap.get(cacheKey);
        if (cacheEpoch >= srcEpoch) {
            result = true;
            break;
        } else {
            Dep* search = next->incomingEdges.get(srcID);
            if (search && search->epoch >= srcEpoch) {
                result = true;
                break;
            }
            if (next->parent && next->parent->id == srcID &&
                next->parentEpoch >= srcEpoch) {
                result = true;
                break;
            }
            if (next->parent && next->parent->id != srcID && next->parent->depth >= srcNode->depth) {
                if (accessedNodes.find(next->parent->id) ==
                    accessedNodes.end()) {
                    accessedNodes.insert(next->parent->id);
                    q.push(next->parent);
                }
            }
            for (auto it = next->incomingEdges.begin(),
                      ie = next->incomingEdges.end();
                 it != ie; ++it) {
                Node* ancestor = (*it).second->src;
                assert(ancestor);
                if (ancestor->id == srcID) {
                    continue;
                }
                if (ancestor->depth < srcNode->depth) {
                    continue;
                }
                while ((ancestor->type == Node::EVENT &&
                        ancestor->incomingEdges.getSize() == 1) ||
                       (ancestor->type == Node::TASK &&
                        ancestor->incomingEdges.getSize() == 0)) {
                    uint32_t ancestorEpoch;
                    if (ancestor->type == Node::EVENT) {
                        auto it = ancestor->incomingEdges.begin();
                        ancestor = (*it).second->src;
                        ancestorEpoch = (*it).second->epoch;
                    } else {
                        ancestorEpoch = ancestor->parentEpoch;
                        ancestor = ancestor->parent;
                    }
                    if (!ancestor) {
                        break;
                    }
                    if (ancestor->id == srcID) {
                        if (ancestorEpoch >= srcEpoch) {
                            result = true;
                            updateCache(srcID, srcEpoch, dstID);
                            return result;
                        } else {
                            break;
                        }
                    }
                }
                if (ancestor && ancestor->id != srcID &&
                    accessedNodes.find(ancestor->id) == accessedNodes.end()) {
                    accessedNodes.insert(ancestor->id);
                    q.push(ancestor);
                }
            }
        }
    }

    if (result) {
        updateCache(srcID, srcEpoch, dstID);
    }
    return result;
}

bool ComputationGraph::isReachable(
    std::unordered_map<uint64_t, uint32_t>& srcNodes, uint64_t dstID,
    uint32_t dstEpoch) {
    // auto it = srcNodes.find(dstID);
    // if (it != srcNodes.end()) {
    // srcNodes.erase(it);
    //}
    // Node* dstNode = nodeMap.get(dstID);
    // bool result = false;
    // std::queue<Node*> q;
    // std::set<uint64_t> accessedNodes;
    // q.push(dstNode);
    // accessedNodes.insert(dstID);
    // while (!q.empty()) {
    // Node* next = q.front();
    // q.pop();
    // if (next->parent) {
    // auto it = srcNodes.find(next->parent->id);
    // if (it != srcNodes.end() && it->second <= next->parentEpoch) {
    // srcNodes.erase(it);
    //}
    //}
    // for (auto di = next->incomingEdges.begin(), de =
    // next->incomingEdges.end(); di != de; ++di) {  Dep* dep = (*di).second; if
    // (dep->src->type == Node::TASK) {  auto it = srcNodes.find(dep->src->id);
    // if (it != srcNodes.end() && it->second <= dep->epoch) {
    // srcNodes.erase(it);
    //}
    //}
    //}
    // if (srcNodes.empty()) {
    // result = true;
    // break;
    //}
    // if (next->parent) {
    // if (accessedNodes.find(next->parent->id) == accessedNodes.end()) {
    // accessedNodes.insert(next->parent->id);
    // q.push(next->parent);
    //}
    //}
    // for (auto di = next->incomingEdges.begin(), de =
    // next->incomingEdges.end(); di != de; ++di) {  Node* ancestor =
    // (*di).second->src;  while (ancestor->type == Node::EVENT &&
    // ancestor->incomingEdges.getSize() == 1) {  auto it =
    // ancestor->incomingEdges.begin();  ancestor = (*it).second->src;
    //}
    // if (accessedNodes.find(ancestor->id) != accessedNodes.end()) {
    // accessedNodes.insert(ancestor->id);
    // q.push(ancestor);
    //}
    //}
    //}
    // return result;
    return true;
}

void ComputationGraph::updateCache(uint64_t srcID, uint32_t srcEpoch,
                                   uint64_t dstID) {
    uint64_t cacheKey = generateCacheKey(srcID, dstID);
    if (cacheMap.get(cacheKey) < srcEpoch) {
        cacheMap.put(cacheKey, srcEpoch);
    }
}

inline void ComputationGraph::updateTaskFinalEpoch(uint64_t taskID,
                                                   uint32_t epoch) {
    epochMap.put(taskID, epoch);
}

#ifdef OUTPUT_CG
void ComputationGraph::toDot(std::ostream& out) {
#ifdef DEBUG
    cout << "CG2Dot" << std::endl;
#endif
    cout << "node num = " << nodeMap.getSize() << std::endl;
    // used to filter redundant nodes, since we may point multiple ID to a
    // single node, for instance, we point an output event's ID to the
    // associated task  std::set<Node*> accessedNode;
    out << "digraph ComputationGraph {" << std::endl;
    for (auto ci = nodeMap.begin(), ce = nodeMap.end(); ci != ce; ++ci) {
        Node* node = (*ci).second;
        // if (accessedNode.find(node) == accessedNode.end()) {
        // accessedNode.insert(node);
        //} else {
        // continue;
        //}
        std::string nodeColor =
            nodeColorSchemes.find(node->type)->second.toString();
        if (node->type == Node::TASK) {
            uint32_t finalEpoch = epochMap.get(node->id);
            for (uint32_t i = START_EPOCH + 1; i <= finalEpoch; i++) {
                out << '\"' << node->id << "#" << i << '\"' << nodeColor << ";"
                    << std::endl;
            }
        } else {
            out << '\"' << node->id << '\"' << nodeColor << ";" << std::endl;
        }
    }

    // accessedNode.clear();
    for (auto ci = nodeMap.begin(), ce = nodeMap.end(); ci != ce; ++ci) {
        Node* node = (*ci).second;
        // if (accessedNode.find(node) == accessedNode.end()) {
        // accessedNode.insert(node);
        //} else {
        // continue;
        //}

        if (node->type == Node::TASK) {
            if (node->parent) {
                outputLink(out, node->parent, node->parentEpoch, node,
                           START_EPOCH + 1, Node::SPAWN);
            }
            for (auto di = node->incomingEdges.begin(),
                      de = node->incomingEdges.end();
                 di != de; ++di) {
                Dep* dep = (*di).second;
                outputLink(out, dep->src,
                           (dep->epoch == END_EPOCH ? epochMap.get(dep->src->id)
                                                    : dep->epoch),
                           node, START_EPOCH + 1, Node::JOIN);
            }
            uint32_t finalEpoch = epochMap.get(node->id);
            for (uint32_t i = START_EPOCH + 1; i < finalEpoch; i++) {
                outputLink(out, node, i, node, i + 1, Node::CONTINUE);
            }
        } else {
            for (auto di = node->incomingEdges.begin(),
                      de = node->incomingEdges.end();
                 di != de; ++di) {
                Dep* dep = (*di).second;
                outputLink(out, dep->src,
                           (dep->epoch == END_EPOCH ? epochMap.get(dep->src->id)
                                                    : dep->epoch),
                           node, START_EPOCH + 1, Node::JOIN);
            }
        }
    }
    out << "}";
}

void ComputationGraph::outputLink(std::ostream& out, Node* n1, uint32_t epoch1,
                                  Node* n2, uint32_t epoch2,
                                  Node::EdgeType edgeType) {
    out << '\"' << n1->id;
    if (n1->type == Node::TASK) {
        out << '#' << epoch1;
    }
    out << '\"';
    out << " -> ";
    out << '\"' << n2->id;
    if (n2->type == Node::TASK) {
        out << '#' << epoch2;
    }
    out << '\"';
    out << ' ' << edgeColorSchemes.find(edgeType)->second.toString();
    out << ';' << std::endl;
}
#endif


#ifdef STATISTICS
void ComputationGraph::showStatistics(std::ostream& out) {
   uint64_t taskNum = 0, eventNum = 0, edgeNum = 0;
   for (auto ni = nodeMap.begin(), ne = nodeMap.end(); ni != ne; ++ni) {
        Node* node = (*ni).second;
        if (node->type == Node::TASK) {
            taskNum += 1;
            edgeNum += node->incomingEdges.getSize();
            if (node->parent) {
                edgeNum += 1;
            }
        } else {
            eventNum += 1;
            edgeNum += node->incomingEdges.getSize();
        }
   } 
   out << "task: " << taskNum << std::endl;
   out << "event: " << eventNum << std::endl;
   out << "edge: " << edgeNum << std::endl;
}
#endif

ComputationGraph computationGraph(10000);

////////////////////////////////////////////////////////////////////////////////
//                                Shadow Memory                              //
///////////////////////////////////////////////////////////////////////////////

class AccessRecord {
   public:
    uint64_t taskID;
    uint32_t epoch;
    int32_t ref;
    ADDRINT ip;

   public:
    AccessRecord() : AccessRecord(NULL_ID, START_EPOCH, 0) {}
    AccessRecord(uint64_t taskID, uint32_t epoch, ADDRINT ip)
        : taskID(taskID), epoch(epoch), ref(0), ip(ip) {}
    AccessRecord& operator=(const AccessRecord& other) {
        this->taskID = other.taskID;
        this->epoch = other.epoch;
        this->ip = other.ip;
        this->ref = 0;
        return *this;
    }
    bool isEmpty() { return taskID == NULL_ID; }
    void increaseRef() { ATOMIC::OPS::Increment(&ref, (int32_t)1); }
    void decreaseRef() {
        int oldVal = ATOMIC::OPS::Increment(&ref, (int32_t)-1);
        if (oldVal == 1) {
            delete this;
        }
    }

    std::string getLocation() {
        return getSourceInfo(ip);
    }

    virtual ~AccessRecord() {}
};

class ByteSM {
   private:
    AccessRecord* write;
    std::unordered_map<uint64_t, AccessRecord*> reads;
    PIN_RWMUTEX rwLock;

   public:
    ByteSM() : write(nullptr), reads() {
        if (!PIN_RWMutexInit(&rwLock)) {
            cerr << "Fail to initialize RW lock" << std::endl;
            PIN_ExitProcess(1);
        }
    }
    virtual ~ByteSM() {
        write->decreaseRef();
        for (auto it = reads.begin(), ie = reads.end(); it != ie; it++) {
            it->second->decreaseRef();
        }
        PIN_RWMutexFini(&rwLock);
    }
    bool hasRead() { return !reads.empty(); }
    bool hasWrite() { return write != nullptr; }
    void updateWrite(AccessRecord* other) {
        PIN_RWMutexWriteLock(&rwLock);
        if (this->write) {
            this->write->decreaseRef();
        }
        this->write = other;
        for (auto it = reads.begin(), ie = reads.end(); it != ie; it++) {
            it->second->decreaseRef();
        }
        this->reads.clear();
        PIN_RWMutexUnlock(&rwLock);
        other->increaseRef();
    }
    void updateRead(AccessRecord* other) {
        PIN_RWMutexWriteLock(&rwLock);
        auto it = reads.find(other->taskID);
        if (it == reads.end()) {
            reads.insert(std::make_pair(other->taskID, other));
        } else {
            it->second->decreaseRef();
            it->second = other;
        }
        PIN_RWMutexUnlock(&rwLock);
        other->increaseRef();
    }
    std::unordered_map<uint64_t, AccessRecord*>& getReads() { return reads; }
    AccessRecord* getWrite() { return write; }
    void readLock() { PIN_RWMutexReadLock(&rwLock); }
    void readUnlock() { PIN_RWMutexUnlock(&rwLock); }
    friend void recordMemRead(THREADID tid, void* addr, uint32_t size,
                              ADDRINT sp, ADDRINT ip);
    friend void recordMemWrite(THREADID tid, void* addr, uint32_t size,
                               ADDRINT sp, ADDRINT ip);
};

class DataBlockSM {
   private:
    uintptr_t startAddress;
    uint64_t length;
    ByteSM* byteArray;

   public:
    DataBlockSM(uintptr_t startAddress, uint64_t length)
        : startAddress(startAddress), length(length) {
        byteArray = new ByteSM[length]();
    }
    virtual ~DataBlockSM() {}
    void update(uintptr_t address, unsigned size, AccessRecord* ar,
                bool isRead) {
        // cout << (isRead ? "read" : "write") << endl;
        uint64_t offset = address - startAddress;
        assert(offset >= 0 && offset < length);
        if (isRead) {
            for (unsigned i = 0; i < size; i++) {
                byteArray[offset + i].updateRead(ar);
            }
        } else {
            for (unsigned i = 0; i < size; i++) {
                byteArray[offset + i].updateWrite(ar);
            }
        }
    }

    friend class ThreadLocalStore;
    friend void recordMemRead(THREADID tid, void* addr, uint32_t size,
                              ADDRINT sp, ADDRINT ip);
    friend void recordMemWrite(THREADID tid, void* addr, uint32_t size,
                               ADDRINT sp, ADDRINT ip);
    friend void afterEventSatisfy(THREADID tid, ocrGuid_t edtGuid,
                                  ocrGuid_t eventGuid, ocrGuid_t dataGuid,
                                  uint32_t slot);
    friend void afterDbRelease(THREADID tid, ocrGuid_t edtGuid,
                               ocrGuid_t dbGuid);
};

class ShadowMemory {
   private:
    ConcurrentHashMap<uint64_t, DataBlockSM*, nullptr, IdentityHash<uint64_t> >
        dbMap;

   public:
    ShadowMemory(uint64_t capacity) : dbMap(capacity) {}
    virtual ~ShadowMemory() {}
    void insertDB(uint64_t id, DataBlockSM* db) { dbMap.put(id, db); }
    DataBlockSM* getDB(uint64_t id) { return dbMap.get(id); }
};

ShadowMemory sm(10000);

////////////////////////////////////////////////////////////////////////////////
//                            Thread Local Storage                           //
///////////////////////////////////////////////////////////////////////////////

class ThreadLocalStore {
   private:
    uint64_t taskID;
    uint32_t taskEpoch;
    std::vector<DataBlockSM*> acquiredDB;

   public:
    void initialize(uint64_t taskID, uint32_t depc, ocrEdtDep_t* depv) {
        this->taskID = taskID;
        this->taskEpoch = START_EPOCH;
        initializeAcquiredDB(depc, depv);
    }
    void cleanStaleData() {
        this->taskID = NULL_ID;
        this->taskEpoch = START_EPOCH;
        acquiredDB.clear();
    }
    void increaseEpoch() { this->taskEpoch++; }
    uint32_t getEpoch() { return this->taskEpoch; }
    uint64_t getTaskID() {return this->taskID; }
    void insertDB(DataBlockSM* db) {
        int offset;
        bool isContain = searchDB(db->startAddress, nullptr, &offset);
        assert(!isContain);
        acquiredDB.insert(acquiredDB.begin() + offset, db);
    }
    DataBlockSM* getDB(uintptr_t addr) {
        DataBlockSM* db;
        searchDB(addr, &db, nullptr);
        return db;
    }
    void removeDB(DataBlockSM* db) {
        int offset;
        bool isContain = searchDB(db->startAddress, nullptr, &offset);
        // assert(isContain);
        if (isContain) {
            acquiredDB.erase(acquiredDB.begin() + offset);
        }
    }

    void initializeAcquiredDB(uint32_t dpec, ocrEdtDep_t* depv);

   private:
    bool searchDB(uintptr_t addr, DataBlockSM** ptr, int* offset);

    friend void recordMemRead(THREADID tid, void* addr, uint32_t size,
                              ADDRINT sp, ADDRINT ip);
    friend void recordMemWrite(THREADID tid, void* addr, uint32_t size,
                               ADDRINT sp, ADDRINT ip);
};

void ThreadLocalStore::initializeAcquiredDB(uint32_t depc, ocrEdtDep_t* depv) {
    acquiredDB.reserve(2 * depc);
    for (u32 i = 0; i < depc; i++) {
        if (depv[i].ptr) {
            DataBlockSM* db = sm.getDB(depv[i].guid.guid);
            assert(db != nullptr);
            assert(db->startAddress == (uintptr_t)depv[i].ptr);
            acquiredDB.push_back(db);
        }
    }
    sort(acquiredDB.begin(), acquiredDB.end(),
         [](DataBlockSM* a, DataBlockSM* b) {
             return a->startAddress < b->startAddress;
         });
}

bool ThreadLocalStore::searchDB(uintptr_t addr, DataBlockSM** ptr,
                                int* offset) {
#ifdef DEBUG
//*out << "search DB" << std::endl;
#endif
    int start = 0, end = acquiredDB.size() - 1;
    bool isFind = false;
    while (start <= end) {
        int next = (start + end) / 2;
        DataBlockSM* db = acquiredDB[next];
        if (addr >= db->startAddress) {
            if (addr < db->startAddress + db->length) {
                // address is inside current db
                isFind = true;
                if (ptr) {
                    *ptr = db;
                }
                if (offset) {
                    *offset = next;
                }
                break;
            } else {
                // addess is after current db
                start = next + 1;
            }
        } else {
            // address is before current db
            end = next - 1;
        }
    }

    if (!isFind) {
        if (ptr) {
            *ptr = nullptr;
        }
        if (offset) {
            *offset = start;
        }
    }

#ifdef DEBUG
        //*out << "search DB end " << (isFind ? "found" : "unfound") <<
        // std::endl;
#endif
    return isFind;
}

////////////////////////////////////////////////////////////////////////////////
//                  API Call & Runtime Event Instrumentation                 //
///////////////////////////////////////////////////////////////////////////////

void preEdt(THREADID tid, ocrGuid_t edtGuid, uint32_t paramc, uint64_t* paramv,
            uint32_t depc, ocrEdtDep_t* depv, uint64_t* dbSizev) {
#ifdef DEBUG
    *out << "preEdt" << std::endl;
#endif
    uint64_t taskID = guidMap.get(edtGuid.guid);
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    tls->initialize(taskID, depc, depv);
    tls->increaseEpoch();
    Node* task = computationGraph.getNode(taskID);
    task->calculateDepth();
#ifdef DEBUG
    *out << "preEdt finish" << std::endl;
#endif
}

// depv is always NULL
void afterEdtCreate(THREADID tid, ocrGuid_t guid, ocrGuid_t templateGuid,
                    uint32_t paramc, uint64_t* paramv, uint32_t depc,
                    ocrGuid_t* depv, uint16_t properties, ocrGuid_t outputEvent,
                    ocrGuid_t parent) {
#ifdef DEBUG
    *out << "afterEdtCreate" << std::endl;
#endif
    if (depc >= 0xFFFFFFFE) {
        cerr << "depc is invalid" << std::endl;
        exit(0);
    }
    uint64_t taskID = generateTaskID();
    guidMap.put(guid.guid, taskID);
    Task* parentTask = nullptr;
    uint32_t parentEpoch = END_EPOCH;
    if (!isNullGuid(parent)) {
        uint64_t parentID = guidMap.get(parent.guid);
        parentTask = static_cast<Task*>(computationGraph.getNode(parentID));
        ThreadLocalStore* tls =
            static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
        parentEpoch = tls->getEpoch();
        tls->increaseEpoch();
    }
    Task* task = new Task(taskID, parentTask, parentEpoch);
    computationGraph.insert(taskID, task);
    if (!isNullGuid(outputEvent)) {
        uint64_t eventID = guidMap.get(outputEvent.guid);
        // if (properties == EDT_PROP_FINISH) {
        // Node* finishEvent = computationGraph.getNode(eventID);
        // Dep dep(task);
        // finishEvent->addDeps(taskID, dep);
        //} else {
        // computationGraph.insert(eventID, task);
        //}
        Node* associatedEvent = computationGraph.getNode(eventID);
        Dep dep(task);
        associatedEvent->addDeps(taskID, dep);
    }
#ifdef DEBUG
    *out << "afterEdtCreate finish" << std::endl;
#endif
}

void afterDbCreate(THREADID tid, ocrGuid_t guid, void* addr, uint64_t len,
                   uint16_t flags, ocrInDbAllocator_t allocator) {
#ifdef DEBUG
    *out << "afterDbCreate" << std::endl;
#endif
    DataBlockSM* newDB = new DataBlockSM((uintptr_t)addr, len);
    sm.insertDB(guid.guid, newDB);
    // new created DB is acquired by current EDT instantly
    //ThreadLocalStore* tls =
        //static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    //tls->insertDB(newDB);
#ifdef DEBUG
    cout << "afterDbCreate finish" << std::endl;
#endif
}

void afterEventCreate(ocrGuid_t guid, ocrEventTypes_t eventType,
                      uint16_t properties) {
#ifdef DEBUG
    *out << "afterEventCreate" << std::endl;
#endif
    uint64_t eventID = generateTaskID();
    guidMap.put(guid.guid, eventID);
    Event* event = new Event(eventID);
    computationGraph.insert(eventID, event);
#ifdef DEBUG
    *out << "afterEventCreate finish" << std::endl;
#endif
}

void afterAddDependence(THREADID tid, ocrGuid_t source, ocrGuid_t destination, uint32_t slot,
                        ocrDbAccessMode_t mode) {
#ifdef DEBUG
    *out << "afterAddDependence" << std::endl;
#endif
    if (!isNullGuid(source)) {
        uint64_t dstID = guidMap.get(destination.guid);
        uint64_t srcID = guidMap.get(source.guid);
        if (srcID != NULL_ID && dstID != NULL_ID) {
            Node* dst = computationGraph.getNode(dstID);
            Node* src = computationGraph.getNode(srcID);
            Dep dep(src);
            assert(dstID != NULL_ID);
            assert(dst != nullptr);
            assert(srcID != NULL_ID);
            assert(src != nullptr);
            dst->addDeps(srcID, dep);
        }
        
        //data dependence
        if (srcID == NULL_ID && dstID != NULL_ID) {
            Node* dst = computationGraph.getNode(dstID);
            ThreadLocalStore* tls = static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
            //bugs----between two tasks executing on the same thread continuously, there can be other code snippet executing in the thread
            if (tls->getTaskID() != NULL_ID) {
                Node* current = computationGraph.getNode(tls->getTaskID());
                Dep dep(current, tls->getEpoch());
                tls->increaseEpoch();
                dst->addDeps(tls->getTaskID(), dep);
            }
        }
    }
#ifdef DEBUG
    *out << "afterAddDependence finish" << std::endl;
#endif
}

void afterEventSatisfy(THREADID tid, ocrGuid_t edtGuid, ocrGuid_t eventGuid,
                       ocrGuid_t dataGuid, uint32_t slot) {
#ifdef DEBUG
    *out << "afterEventSatisfy" << std::endl;
#endif
    // According to spec, event satisfication should be treated that it happens
    // at once when all dependences are satisfied, even if in some case the
    // satisfication may be delayed.
    uint64_t triggerTaskID = guidMap.get(edtGuid.guid);
    uint64_t eventID = guidMap.get(eventGuid.guid);
    Task* triggerTask =
        static_cast<Task*>(computationGraph.getNode(triggerTaskID));
    Event* event = static_cast<Event*>(computationGraph.getNode(eventID));
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    Dep dep(triggerTask, tls->getEpoch());
    tls->increaseEpoch();
    assert(event != nullptr);
    event->addDeps(triggerTaskID, dep);
    // after satisfy, the data block become shared
    // DataBlockSM* db = sm.getDB(dataGuid.guid);
    // if (!tls->getDB(db->startAddress)) {
    // tls->insertDB(db);
    //}
#ifdef DEBUG
    *out << "afterEventSatisfy finish" << std::endl;
#endif
}

//void afterEventPropagate(ocrGuid_t eventGuid) {
//#ifdef DEBUG
 //*out << "afterEventPropagate" << std::endl;
//#endif
//
//#ifdef DEBUG
 //*out << "afterEventPropagate finish" << std::endl;
//#endif
//}

void afterEdtTerminate(THREADID tid, ocrGuid_t edtGuid) {
#ifdef DEBUG
    *out << "afterEdtTerminate" << std::endl;
#endif
    uint64_t taskID = guidMap.get(edtGuid.guid);
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    computationGraph.updateTaskFinalEpoch(taskID, tls->getEpoch());
    tls->cleanStaleData();
#ifdef DEBUG
    *out << "afterEdtTerminate finish" << std::endl;
#endif
}

void afterDbRelease(THREADID tid, ocrGuid_t edtGuid, ocrGuid_t dbGuid) {
#ifdef DEBUG
    *out << "afterDbRelease" << std::endl;
#endif
    DataBlockSM* db = sm.getDB(dbGuid.guid);
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    tls->removeDB(db);
#ifdef DEBUG
    *out << "afterDbRelease finish" << std::endl;
#endif
}

void afterDbDestroy(THREADID tid, ocrGuid_t dbGuid) {
#ifdef DEBUG
    *out << "afterDbDestroy" << std::endl;
#endif
    DataBlockSM* db = sm.getDB(dbGuid.guid);
    if (db) {
        ThreadLocalStore* tls =
            static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
        tls->removeDB(db);
        // cout << "thread " << tid << " delete " << db->startAddress << endl;
        delete db;
    }
#ifdef DEBUG
    *out << "afterDbDestroy finish" << std::endl;
#endif
}

void threadStart(THREADID tid, CONTEXT* ctxt, int32_t flags, void* v) {
    ThreadLocalStore* tls = new ThreadLocalStore();
    if (PIN_SetThreadData(tls_key, tls, tid) == FALSE) {
        cerr << "PIN_SetThreadData failed" << std::endl;
        PIN_ExitProcess(1);
    }
}

void threadFini(THREADID tid, const CONTEXT* ctxt, int32_t code, void* v) {
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    if (tls) {
        delete tls;
    }
}

void fini(int32_t code, void* v) {
#ifdef DEBUG
    *out << "fini" << std::endl;
#endif

#ifdef MEASURE_TIME
    gettimeofday(&program_end, nullptr);
    time_t time_span = (program_end.tv_sec - program_start.tv_sec) * 1000000 + (program_end.tv_usec - program_start.tv_usec);
    *out << "elapsed time: " << time_span << " us" << std::endl;
#endif

#ifdef OUTPUT_CG
    ofstream dotFile;
    dotFile.open("cg.dot");
    computationGraph.toDot(dotFile);
    dotFile.close();
#endif

#ifdef STATISTICS
    computationGraph.showStatistics(*out);
#endif
    // errorFile.close();
    // logFile.close();
}

void overload(IMG img, void* v) {
#ifdef DEBUG
    *out << "img: " << IMG_Name(img) << std::endl;
#endif

    if (isOCRLibrary(img)) {
        // monitor mainEdt
        // RTN mainEdtRTN = RTN_FindByName(img, "mainEdt");
        // if (RTN_Valid(mainEdtRTN)) {
        //#ifdef DEBUG
        // *out << "instrument mainEdt" << endl;
        //#endif
        // RTN_Open(mainEdtRTN);
        // RTN_InsertCall(mainEdtRTN, IPOINT_BEFORE,
        //(AFUNPTR)argsMainEdt,
        // IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
        // IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
        // IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
        // IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
        // IARG_END);
        // RTN_Close(mainEdtRTN);
        //}

        // replace notifyEdtStart
        RTN rtn = RTN_FindByName(img, "notifyEdtStart");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEdtStart" << std::endl;
#endif
            PROTO proto_notifyEdtStart = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtStart",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(uint32_t),
                PIN_PARG(uint64_t*), PIN_PARG(uint32_t), PIN_PARG(ocrEdtDep_t*),
                PIN_PARG(uint64_t*), PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(preEdt), IARG_PROTOTYPE, proto_notifyEdtStart,
                IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 4, IARG_FUNCARG_ENTRYPOINT_VALUE,
                5, IARG_END);
            PROTO_Free(proto_notifyEdtStart);
        }
        // replace notifyEdtCreate
        rtn = RTN_FindByName(img, "notifyEdtCreate");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEdtCreate" << std::endl;
#endif
            PROTO proto_notifyEdtCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG(uint32_t), PIN_PARG(uint64_t*), PIN_PARG(uint32_t),
                PIN_PARG(ocrGuid_t*), PIN_PARG(uint16_t),
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(afterEdtCreate), IARG_PROTOTYPE,
                proto_notifyEdtCreate, IARG_THREAD_ID,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE,
                1, IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 3, IARG_FUNCARG_ENTRYPOINT_VALUE,
                4, IARG_FUNCARG_ENTRYPOINT_VALUE, 5,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 6, IARG_FUNCARG_ENTRYPOINT_VALUE,
                7, IARG_FUNCARG_ENTRYPOINT_VALUE, 8, IARG_END);
            PROTO_Free(proto_notifyEdtCreate);
        }

        // replace notidyDbCreate
        rtn = RTN_FindByName(img, "notifyDbCreate");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyDbCreate" << std::endl;
#endif
            PROTO proto_notifyDbCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyDbCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(void*),
                PIN_PARG(uint64_t), PIN_PARG(uint16_t),
                PIN_PARG_ENUM(ocrInDbAllocator_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterDbCreate), IARG_PROTOTYPE,
                                 proto_notifyDbCreate, IARG_THREAD_ID,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 4, IARG_END);
            PROTO_Free(proto_notifyDbCreate);
        }

        // replace notifyEventCreate
        rtn = RTN_FindByName(img, "notifyEventCreate");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEventCreate" << std::endl;
#endif
            PROTO proto_notifyEventCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_ENUM(ocrEventTypes_t),
                PIN_PARG(uint16_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterEventCreate), IARG_PROTOTYPE,
                                 proto_notifyEventCreate,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 2, IARG_END);
            PROTO_Free(proto_notifyEventCreate);
        }

        // replace notifyAddDependence
        rtn = RTN_FindByName(img, "notifyAddDependence");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyAddDependence" << std::endl;
#endif
            PROTO proto_notifyAddDependence = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyAddDependence",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG(uint32_t), PIN_PARG_ENUM(ocrDbAccessMode_t),
                PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(afterAddDependence), IARG_PROTOTYPE,
                proto_notifyAddDependence, IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3, IARG_END);
            PROTO_Free(proto_notifyAddDependence);
        }

        // replace notifyEventSatisfy
        rtn = RTN_FindByName(img, "notifyEventSatisfy");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEventSatisfy" << std::endl;
#endif
            PROTO proto_notifyEventSatisfy = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventSatisfy",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(uint32_t),
                PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterEventSatisfy),
                                 IARG_PROTOTYPE, proto_notifyEventSatisfy,
                                 IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE,
                                 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 3, IARG_END);
            PROTO_Free(proto_notifyEventSatisfy);
        }

        // replace notifyShutdown
        // rtn = RTN_FindByName(img, "notifyShutdown");
        // if (RTN_Valid(rtn)) {
        //#ifdef DEBUG
        // *out << "replace notifyShutdown" << std::endl;
        //#endif
        // PROTO proto_notifyShutdown =
        // PROTO_Allocate(PIN_PARG(void), CALLINGSTD_DEFAULT,
        //"notifyShutdown", PIN_PARG_END());
        // RTN_ReplaceSignature(rtn, AFUNPTR(fini), IARG_PROTOTYPE,
        // proto_notifyShutdown, IARG_END);
        // PROTO_Free(proto_notifyShutdown);
        //}

        // replace notifyDbDestroy
        rtn = RTN_FindByName(img, "notifyDbDestroy");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyDbDestroy" << std::endl;
#endif
            PROTO proto_notifyDbDestroy = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyDbDestroy",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterDbDestroy), IARG_PROTOTYPE,
                                 proto_notifyDbDestroy, IARG_THREAD_ID,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
            PROTO_Free(proto_notifyDbDestroy);
        }

        // replace notifyDbRelease
        rtn = RTN_FindByName(img, "notifyDbRelease");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyDbRelease" << std::endl;
#endif
            PROTO proto_notifyDbRelease =
                PROTO_Allocate(PIN_PARG(void), CALLINGSTD_DEFAULT,
                               "notifyDbRelease", PIN_PARG_AGGREGATE(ocrGuid_t),
                               PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterDbRelease), IARG_PROTOTYPE,
                                 proto_notifyDbRelease, IARG_THREAD_ID,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
            PROTO_Free(proto_notifyDbRelease);
        }

         // replace notifyEventPropagate
         //rtn = RTN_FindByName(img, "notifyEventPropagate");
         //if (RTN_Valid(rtn)) {
        //#ifdef DEBUG
        //*out << "replace notifyEventPropagate" << std::endl;
        //#endif
         //PROTO proto_notifyEventPropagate = PROTO_Allocate(
         //PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventPropagate",
         //PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
         //RTN_ReplaceSignature(rtn, AFUNPTR(afterEventPropagate),
         //IARG_PROTOTYPE, proto_notifyEventPropagate,
         //IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
         //PROTO_Free(proto_notifyEventPropagate);
        //}

        // replace notifyEdtTerminate
        rtn = RTN_FindByName(img, "notifyEdtTerminate");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEdtTerminate";
#endif
            PROTO proto_notifyEdtTerminate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtTerminate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterEdtTerminate),
                                 IARG_PROTOTYPE, proto_notifyEdtTerminate,
                                 IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE,
                                 0, IARG_END);
            PROTO_Free(proto_notifyEdtTerminate);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
//                Memory Operation Instrumentation                           //
///////////////////////////////////////////////////////////////////////////////

void recordMemRead(THREADID tid, void* addr, uint32_t size, ADDRINT sp,
                   ADDRINT ip) {
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    DataBlockSM* db = tls->getDB((uintptr_t)addr);
    std::unordered_map<uint64_t, uint32_t> epochs;
    std::set<AccessRecord*> ars;
#ifdef OUTPUT_SOURCE
    std::unordered_map<uint64_t, ADDRINT> ips;
#endif
    if (db) {
        uintptr_t offset = (uintptr_t)addr - db->startAddress;
        for (uintptr_t i = 0; i < size; i++) {
            ByteSM& byteSM = db->byteArray[offset + i];
            byteSM.readLock();
            if (byteSM.hasWrite()) {
                AccessRecord* write = byteSM.getWrite();
                if (ars.find(write) == ars.end()) {
                    ars.insert(write);
                    auto it = epochs.find(write->taskID);
                    if (it == epochs.end()) {
                        epochs.insert(
                            std::make_pair(write->taskID, write->epoch));
#ifdef OUTPUT_SOURCE
                        ips.insert(std::make_pair(write->taskID, write->ip));
#endif
                    } else if (write->epoch > it->second) {
                        it->second = write->epoch;
#ifdef OUTPUT_SOURCE
                        ips[write->taskID] = write->ip;
#endif
                    }
                }
            }
            byteSM.readUnlock();
        }

        AccessRecord* ar = new AccessRecord(tls->taskID, tls->taskEpoch, ip);
        db->update((uintptr_t)addr, size, ar, true);
#ifdef DETECT_RACE
        // if (epochs.size() == 1) {
        // auto ei = epochs.begin();
        // if (!computationGraph.isReachable(ei->first, ei->second, tls->taskID,
        // tls->taskEpoch)) { *out << "read-write race" << std::endl;
        // PIN_ExitProcess(1);
        //}
        //} else {
        // if (!computationGraph.isReachable(epochs, tls->taskID,
        // tls->taskEpoch)) { *out << "read-write race" << std::endl;
        // PIN_ExitProcess(1);
        //}
        //}
        for (auto ei = epochs.begin(), ee = epochs.end(); ei != ee; ei++) {
            if (!computationGraph.isReachable(ei->first, ei->second,
                                              tls->taskID, tls->taskEpoch)) {
                *out << "write-read race" << std::endl;
                *out << ei->first << "#" << ei->second << " is conflict with "
                     << tls->taskID << "#" << tls->taskEpoch << std::endl;
#ifdef OUTPUT_SOURCE
                *out << "write: " << getSourceInfo(ips[ei->first]) << std::endl; 
                *out << "read:  " << getSourceInfo(ip) << std::endl;
#endif

#ifdef OUTPUT_CG
                 ofstream dotFile;
                 dotFile.open("cg.dot");
                 computationGraph.toDot(dotFile);
                 dotFile.close();
#endif
                PIN_ExitProcess(1);
            }
        }
#endif
    }
}

void recordMemWrite(THREADID tid, void* addr, uint32_t size, ADDRINT sp,
                    ADDRINT ip) {
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    DataBlockSM* db = tls->getDB((uintptr_t)addr);
    std::unordered_map<uint64_t, uint32_t> epochs;
    std::set<AccessRecord*> ars;
#ifdef OUTPUT_SOURCE
    std::unordered_map<uint64_t, ADDRINT> ips;
#endif
    if (db) {
        uintptr_t offset = (uintptr_t)addr - db->startAddress;
        for (uintptr_t i = 0; i < size; i++) {
            ByteSM& byteSM = db->byteArray[offset + i];
            byteSM.readLock();
            if (byteSM.hasWrite()) {
                AccessRecord* write = byteSM.getWrite();
                if (ars.find(write) == ars.end()) {
                    ars.insert(write);
                    auto it = epochs.find(write->taskID);
                    if (it == epochs.end()) {
                        epochs.insert(
                            std::make_pair(write->taskID, write->epoch));
#ifdef OUTPUT_SOURCE
                        ips.insert(std::make_pair(write->taskID, write->ip));
#endif
                    } else if (write->epoch > it->second) {
                        it->second = write->epoch;
#ifdef OUTPUT_SOURCE
                        ips[write->taskID] = write->ip;
#endif
                    }
                }
            }
            if (byteSM.hasRead()) {
                for (auto rt = byteSM.getReads().begin(),
                          re = byteSM.getReads().end();
                     rt != re; ++rt) {
                    AccessRecord* read = rt->second;
                    if (ars.find(read) == ars.end()) {
                        ars.insert(read);
                        auto it = epochs.find(read->taskID);
                        if (it == epochs.end()) {
                            epochs.insert(
                                std::make_pair(read->taskID, read->epoch));
#ifdef OUTPUT_SOURCE
                            ips.insert(std::make_pair(read->taskID, read->ip));
#endif
                        } else if (read->epoch > it->second) {
                            it->second = read->epoch;
#ifdef OUTPUT_SOURCE
                            ips[read->taskID] = read->ip;
#endif
                        }
                    }
                }
            }
            byteSM.readUnlock();
        }

        AccessRecord* ar = new AccessRecord(tls->taskID, tls->taskEpoch, ip);
        db->update(uintptr_t(addr), size, ar, false);

#ifdef DETECT_RACE
        // if (epochs.size() == 1) {
        // auto ei = epochs.begin();
        // if (!computationGraph.isReachable(ei->first, ei->second,
        // tls->taskID, tls->taskEpoch)) {
        //*out << "write race" << std::endl;
        // PIN_ExitProcess(1);
        //}
        //} else {
        // if (!computationGraph.isReachable(epochs, tls->taskID,
        // tls->taskEpoch)) {
        //*out << "write race" << std::endl;
        // PIN_ExitProcess(1);
        //}
        //}
        for (auto ei = epochs.begin(), ee = epochs.end(); ei != ee; ei++) {
            if (!computationGraph.isReachable(ei->first, ei->second,
                                              tls->taskID, tls->taskEpoch)) {
                *out << "write race" << std::endl;
                *out << ei->first << "#" << ei->second << " is conflict with "
                     << tls->taskID << "#" << tls->taskEpoch << std::endl;
#ifdef OUTPUT_SOURCE
                *out << "previous: " << getSourceInfo(ips[ei->first]) << std::endl; 
                *out << "write:  " << getSourceInfo(ip) << std::endl;
#endif

#ifdef OUTPUT_CG
                 ofstream dotFile;
                 dotFile.open("cg.dot");
                 computationGraph.toDot(dotFile);
                 dotFile.close();
#endif
                PIN_ExitProcess(1);
            }
        }
#endif
    }
}

void instrumentInstruction(INS ins) {
    if (isIgnorableIns(ins)) return;

    if (INS_IsAtomicUpdate(ins)) return;

    uint32_t memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (uint32_t memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsRead(ins, memOp)) {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)recordMemRead,
                                     IARG_THREAD_ID, IARG_MEMORYOP_EA, memOp,
                                     IARG_MEMORYREAD_SIZE, IARG_REG_VALUE,
                                     REG_STACK_PTR, IARG_INST_PTR, IARG_END);
        }
        // Note that in some architectures a single memory operand can be
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp)) {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)recordMemWrite, IARG_THREAD_ID,
                IARG_MEMORYOP_EA, memOp, IARG_MEMORYWRITE_SIZE, IARG_REG_VALUE,
                REG_STACK_PTR, IARG_INST_PTR, IARG_END);
        }
    }
}

void instrumentRoutine(RTN rtn) {
    RTN_Open(rtn);
#ifdef DEBUG
    *out << "rtn: " << RTN_Name(rtn) << std::endl;
#endif
    if (!isOCRCall(rtn)) {
        for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
            instrumentInstruction(ins);
        }
    }
    RTN_Close(rtn);
}

void instrumentImage(IMG img, void* v) {
#ifdef DEBUG
    *out << "instrument image" << std::endl;
#endif
    if (isUserCodeImg(img)) {
        for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
            for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn);
                 rtn = RTN_Next(rtn)) {
                instrumentRoutine(rtn);
            }
        }
    }
#ifdef DEBUG
    *out << "instrument image finish" << std::endl;
#endif
}

int usage() {
    cout << "OCR-Racer is a graph traversal based data race detector for "
            "Open Community Runtime"
         << std::endl;
    cout << "Usage: pin -t [path to OCR-Racer.so] -- [path to OCR "
            "app][arg1][arg2]..."
         << std::endl;
    return 1;
}

void initSkippedLibrary() {
    skippedLibraries.push_back("ld-linux-x86-64.so.2");
    skippedLibraries.push_back("libpthread.so.0");
    skippedLibraries.push_back("libc.so.6");
    skippedLibraries.push_back("libocr_x86.so");
    skippedLibraries.push_back("[vdso]");
}

void init(int argc, char* argv[]) {
    int argi;
    for (argi = 0; argi < argc; argi++) {
        std::string arg = argv[argi];
        if (arg == "--") {
            break;
        }
    }
    if (argi == argc - 1 || argi == argc) {
        usage();
        exit(1);
    }
    userCodeImg = argv[argi + 1];
    *out << "User image is " << userCodeImg << std::endl;
    initSkippedLibrary();

    // logFile.open("log.txt");
    // errorFile.open("error.txt");
}

int main(int argc, char* argv[]) {
    // uint64_t t1 = 0x00000000FFFF1111;
    // uint64_t t2 = 0x00000000FFFFFFFF;
    // cout << ((generateCacheKey(t1, t2) == 0xFFFF1111FFFFFFFF) ? "true" :
    // "false") << endl;  uint64_t t3 = generateTaskID();  uint64_t t4 =
    // generateTaskID();  uint64_t expected = 0x0000000100000002;  cout <<
    // ((generateCacheKey(t3, t4) == expected) ? "true" : "false") << endl;
    // cout << std::hex << t3 << endl;
    // cout << std::hex << t4 << endl;
    // cout << std::hex << generateCacheKey(t3, t4) << endl;

    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) {
        return usage();
    }
    init(argc, argv);

    tls_key = PIN_CreateThreadDataKey(NULL);
    if (tls_key == INVALID_TLS_KEY) {
        cerr << "number of already allocated keys reached the "
                "MAX_CLIENT_TLS_KEYS limit"
             << std::endl;
        PIN_ExitProcess(1);
    }
#ifdef GRAPH_CONSTRUCTION    
    PIN_AddThreadStartFunction(threadStart, NULL);
    PIN_AddThreadFiniFunction(threadFini, NULL);
    IMG_AddInstrumentFunction(overload, 0);
#endif

    PIN_AddFiniFunction(fini, 0);

#ifdef INSTRUMENT
    IMG_AddInstrumentFunction(instrumentImage, 0);
#endif

#ifdef MEASURE_TIME
    gettimeofday(&program_start, nullptr);
#endif
    PIN_StartProgram();
    return 0;
}
