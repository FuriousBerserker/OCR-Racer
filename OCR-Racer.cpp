
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
#include <string>
#include <unordered_map>
#include "pin.H"
#define __USE_PIN_CAS
#include "ConcurrentHashMap.hpp"
#include "atomic/ops-enum.hpp"
#include "atomic/ops.hpp"
#include "ocr-types.h"

//#define DEBUG
#define INSTRUMENT
#define DETECT_RACE
#define MEASURE_TIME
//#define OUTPUT_CG

class Node;
class Task;
class DataBlock;
class Event;
class ComputationMap;

#ifdef MEASURE_TIME
clock_t program_start, program_end;
#endif

const uint32_t NULL_ID = 0;

const uint32_t START_EPOCH = 0;
const uint32_t END_EPOCH = 0xFFFFFFFF;

uint32_t nextTaskID = 1;

// guid ---> task ID map
ConcurrentHashMap<uint64_t, uint64_t, NULL_ID, IdentityHash<uint64_t> > guidMap(
    10000);

// library list
vector<string> skippedLibraries;

// user code image name
string userCodeImg;

// key for accessing TLS storage in the threads. initialized once in main()
static TLS_KEY tls_key = INVALID_TLS_KEY;

// output stream
std::ostream* out = &cout;
std::ostream* err = &cerr;
// std::ofstream logFile, errorFile;
// std::ostream *out = &logFile;
// std::ostream *err = &errorFile;
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

inline bool isOCRLibrary(IMG img) {
    string ocrLibraryName = "libocr_x86.so";
    string imageName = IMG_Name(img);
    if (isEndWith(imageName, ocrLibraryName)) {
        return true;
    } else {
        return false;
    }
}

/**
 * Test whether guid id NULL_GUID
 */
bool isNullGuid(ocrGuid_t guid) {
    if (guid.guid == NULL_GUID.guid) {
        return true;
    } else {
        return false;
    }
}

#ifdef OUTPUT_CG
class ColorScheme {
   private:
    string color;
    string style;

   public:
    ColorScheme() {}
    ColorScheme(std::string color, std::string style)
        : color(color), style(style) {}
    virtual ~ColorScheme() {}
    string toString() { return "[color=" + color + ", style=" + style + "]"; }
};
#endif

class Dep {
   public:
    Node* src;
    uint32_t epoch;
    Dep(Node* src, uint32_t epoch = END_EPOCH) : src(src), epoch(epoch) {}
    virtual ~Dep() {}
};

class Node {
   public:
    enum Type { TASK, DB, EVENT };
    enum EdgeType { SPAWN, JOIN, CONTINUE };

   protected:
    std::unordered_map<uint64_t, Dep> incomingEdges;
    Task* parent;
    uint32_t parentEpoch;
    Type type;

   public:
    uint64_t id;

   public:
    Node(uint64_t id, Type type, Task* parent, uint32_t parentEpoch);
    virtual ~Node();
    void addDeps(uint64_t id, Dep& dep);

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
    void updateCache(uint64_t srcID, uint32_t srcEpoch, uint32_t dstID);
    void updateTaskFinalEpoch(uint64_t taskID, uint32_t epoch);
#ifdef OUTPUT_CG
    void toDot(std::ostream& out);
#endif
   private:
#ifdef OUTPUT_CG
    std::map<Node::Type, ColorScheme> nodeColorSchemes;
    std::map<Node::EdgeType, ColorScheme> edgeColorSchemes;
    void outputLink(ostream& out, Node* n1, u16 epoch1, Node* n2, u16 epoch2,
                    Node::EdgeType edgeType);
#endif
};

class ShadowMemory {};

class ThreadLocalStore {
   public:
    uint64_t taskID;
    uint32_t taskEpoch;
    void initialize(uint64_t taskID) {
        this->taskID = taskID;
        this->taskEpoch = START_EPOCH;
    }
    void increaseEpoch() { this->taskEpoch++; }
    uint32_t getEpoch() { return this->taskEpoch; }
    // vector<DBPage*> acquiredDB;
    // void initializeAcquiredDB(u32 dpec, ocrEdtDep_t* depv);
    // void insertDB(ocrGuid_t& guid);
    // DBPage* getDB(uintptr_t addr);
    // void removeDB(DBPage* dbPage);

    // private:
    // bool searchDB(uintptr_t addr, DBPage** ptr, u64* offset);
};

Node::Node(uint64_t id, Type type, Task* parent, uint32_t parentEpoch)
    : incomingEdges(),
      parent(parent),
      parentEpoch(parentEpoch),
      type(type),
      id(id) {}

Node::~Node() {}

inline void Node::addDeps(uint64_t id, Dep& dep) {
    incomingEdges.insert(std::make_pair(id, dep));
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
    bool result = false;
    queue<Node*> q;
    q.push(dstNode);
    uint64_t cacheKey = generateCacheKey(srcID, dstID);
    uint32_t cacheEpoch = cacheMap.get(cacheKey);
    if (cacheEpoch >= srcEpoch) {
        result = true;
    } else {
        while (!q.empty()) {
            Node* next = q.front();
            q.pop();
            auto search = next->incomingEdges.find(srcID);
            if (search != next->incomingEdges.end() &&
                search->second.epoch >= srcEpoch) {
                result = true;
                break;
            }
            if (next->parent && next->parent->id == srcID &&
                next->parent->parentEpoch >= srcEpoch) {
                result = true;
                break;
            }
            if (next->parent) {
                q.push(next->parent);
            }
            for (auto it = next->incomingEdges.begin(),
                      ie = next->incomingEdges.end();
                 it != ie; it++) {
                 Node* ancestor = it->second.src;
                 while (ancestor->type == Node::EVENT && ancestor->incomingEdges.size() == 1) {
                    ancestor = ancestor->incomingEdges.begin()->second.src; 
                 }
                q.push(ancestor);
            }
        }
    }
    if (result) {
        updateCache(srcID, srcEpoch, dstID);
    }
    return result;
}

void ComputationGraph::updateCache(uint64_t srcID, uint32_t srcEpoch,
                                   uint32_t dstID) {
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
    cout << "CG2Dot" << endl;
#endif
    //used to filter redundant nodes, since we may point multiple ID to a single node, for instance, we point an output event's ID to the associated task
    //std::set<Node*> accessedNode;
    out << "digraph ComputationGraph {" << endl;
    for (auto ci = nodeMap.begin(), ce = nodeMap.end(); ci != ce; ++ci) {
        Node* node = (*ci).second;
        //if (accessedNode.find(node) == accessedNode.end()) {
            //accessedNode.insert(node);
        //} else {
            //continue;
        //}
        string nodeColor = nodeColorSchemes.find(node->type)->second.toString();
        if (node->type == Node::TASK) {
            uint32_t finalEpoch = epochMap.get(node->id);
            for (uint32_t i = START_EPOCH + 1; i <= finalEpoch; i++) {
                out << '\"' << node->id << "#" << i << '\"' << nodeColor << ";"
                    << endl;
            }
        } else {
            out << '\"' << node->id << '\"' << nodeColor << ";" << endl;
        }
    }

    //accessedNode.clear();
    for (auto ci = nodeMap.begin(), ce = nodeMap.end(); ci != ce; ++ci) {
        Node* node = (*ci).second;
        //if (accessedNode.find(node) == accessedNode.end()) {
            //accessedNode.insert(node);
        //} else {
            //continue;
        //}

        if (node->type == Node::TASK) {
            if (node->parent) {
                outputLink(out, node->parent, node->parentEpoch, node,
                           START_EPOCH + 1, Node::SPAWN);
            }
            for (auto di = node->incomingEdges.begin(),
                      de = node->incomingEdges.end();
                 di != de; ++di) {
                Dep& dep = di->second;
                outputLink(out, dep.src,
                           dep.epoch == END_EPOCH ? epochMap.get(dep.src->id)
                                                  : dep.epoch,
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
                Dep& dep = di->second;
                outputLink(out, dep.src,
                           dep.epoch == END_EPOCH ? epochMap.get(dep.src->id)
                                                  : dep.epoch,
                           node, START_EPOCH + 1, Node::JOIN);
            }
        }
    }
    out << "}";
}

void ComputationGraph::outputLink(ostream& out, Node* n1, u16 epoch1, Node* n2,
                                  u16 epoch2, Node::EdgeType edgeType) {
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
    out << ';' << endl;
}
#endif

ComputationGraph computationGraph(10000);

void preEdt(THREADID tid, ocrGuid_t edtGuid, u32 paramc, u64* paramv, u32 depc,
            ocrEdtDep_t* depv, u64* dbSizev) {
#ifdef DEBUG
    *out << "preEdt" << endl;
#endif
    uint64_t taskID = guidMap.get(edtGuid.guid);
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    tls->initialize(taskID);
    tls->increaseEpoch();
#ifdef DEBUG
    *out << "preEdt finish" << endl;
#endif
}

// depv is always NULL
void afterEdtCreate(THREADID tid, ocrGuid_t guid, ocrGuid_t templateGuid,
                    u32 paramc, u64* paramv, u32 depc, ocrGuid_t* depv,
                    u16 properties, ocrGuid_t outputEvent, ocrGuid_t parent) {
#ifdef DEBUG
    *out << "afterEdtCreate" << endl;
#endif
    if (depc >= 0xFFFFFFFE) {
        cerr << "depc is invalid" << endl;
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
        //if (properties == EDT_PROP_FINISH) {
            //Node* finishEvent = computationGraph.getNode(eventID);
            //Dep dep(task);
            //finishEvent->addDeps(taskID, dep);
        //} else {
            //computationGraph.insert(eventID, task);
        //}    
        Node* finishEvent = computationGraph.getNode(eventID);
        Dep dep(task);
        finishEvent->addDeps(taskID, dep);
    }
#ifdef DEBUG
    *out << "afterEdtCreate finish" << endl;
#endif
}

void afterEventCreate(ocrGuid_t guid, ocrEventTypes_t eventType,
                      u16 properties) {
#ifdef DEBUG
    *out << "afterEventCreate" << endl;
#endif
    uint64_t eventID = generateTaskID();
    guidMap.put(guid.guid, eventID);
    Event* event = new Event(eventID);
    computationGraph.insert(eventID, event);
#ifdef DEBUG
    *out << "afterEventCreate finish" << endl;
#endif
}

void afterAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot,
                        ocrDbAccessMode_t mode) {
#ifdef DEBUG
    *out << "afterAddDependence" << endl;
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
    }
#ifdef DEBUG
    *out << "afterAddDependence finish" << endl;
#endif
}

void afterEventSatisfy(THREADID tid, ocrGuid_t edtGuid, ocrGuid_t eventGuid,
                       ocrGuid_t dataGuid, u32 slot) {
#ifdef DEBUG
    cout << "afterEventSatisfy" << endl;
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
    event->addDeps(triggerTaskID, dep);
#ifdef DEBUG
    cout << "afterEventSatisfy finish" << endl;
#endif
}

// void afterEventPropagate(ocrGuid_t eventGuid) {
//#ifdef DEBUG
// cout << "afterEventPropagate" << endl;
//#endif
//
//#ifdef DEBUG
// cout << "afterEventPropagate finish" << endl;
//#endif
//}

void afterEdtTerminate(THREADID tid, ocrGuid_t edtGuid) {
#ifdef DEBUG
    cout << "afterEdtTerminate" << endl;
#endif
    uint64_t taskID = guidMap.get(edtGuid.guid);
    ThreadLocalStore* tls =
        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tls_key, tid));
    computationGraph.updateTaskFinalEpoch(taskID, tls->getEpoch());
#ifdef DEBUG
    cout << "afterEdtTerminate finish" << endl;
#endif
}

void threadStart(THREADID tid, CONTEXT* ctxt, int32_t flags, void* v) {
    ThreadLocalStore* tls = new ThreadLocalStore();
    if (PIN_SetThreadData(tls_key, tls, tid) == FALSE) {
        cerr << "PIN_SetThreadData failed" << endl;
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
    *out << "fini" << endl;
#endif

#ifdef MEASURE_TIME
    program_end = clock();
    double time_span = program_end - program_start;
    time_span /= CLOCKS_PER_SEC;
    *out << "elapsed time: " << time_span << " seconds" << endl;
#endif

#ifdef OUTPUT_CG
    ofstream dotFile;
    dotFile.open("cg.dot");
    computationGraph.toDot(dotFile);
    dotFile.close();
#endif
    // errorFile.close();
    // logFile.close();
}

void overload(IMG img, void* v) {
#ifdef DEBUG
    *out << "img: " << IMG_Name(img) << endl;
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
            *out << "replace notifyEdtStart" << endl;
#endif
            PROTO proto_notifyEdtStart = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtStart",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(u32), PIN_PARG(u64*),
                PIN_PARG(u32), PIN_PARG(ocrEdtDep_t*), PIN_PARG(u64*),
                PIN_PARG_END());
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
            *out << "replace notifyEdtCreate" << endl;
#endif
            PROTO proto_notifyEdtCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG(u32), PIN_PARG(u64*), PIN_PARG(u32),
                PIN_PARG(ocrGuid_t*), PIN_PARG(u16),
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
        // rtn = RTN_FindByName(img, "notifyDbCreate");
        // if (RTN_Valid(rtn)) {
        //#ifdef DEBUG
        //*out << "replace notifyDbCreate" << endl;
        //#endif
        // PROTO proto_notifyDbCreate = PROTO_Allocate(
        // PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyDbCreate",
        // PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(void*), PIN_PARG(u64),
        // PIN_PARG(u16), PIN_PARG_ENUM(ocrInDbAllocator_t),
        // PIN_PARG_END());
        // RTN_ReplaceSignature(
        // rtn, AFUNPTR(afterDbCreate), IARG_PROTOTYPE,
        // proto_notifyDbCreate, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
        // IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
        // 2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
        // IARG_FUNCARG_ENTRYPOINT_VALUE, 4, IARG_END);
        // PROTO_Free(proto_notifyDbCreate);
        //}

        // replace notifyEventCreate
        rtn = RTN_FindByName(img, "notifyEventCreate");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEventCreate" << endl;
#endif
            PROTO proto_notifyEventCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_ENUM(ocrEventTypes_t),
                PIN_PARG(u16), PIN_PARG_END());
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
            *out << "replace notifyAddDependence" << endl;
#endif
            PROTO proto_notifyAddDependence = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyAddDependence",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG(u32), PIN_PARG_ENUM(ocrDbAccessMode_t),
                PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(afterAddDependence), IARG_PROTOTYPE,
                proto_notifyAddDependence, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3, IARG_END);
            PROTO_Free(proto_notifyAddDependence);
        }

        // replace notifyEventSatisfy
        rtn = RTN_FindByName(img, "notifyEventSatisfy");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEventSatisfy" << endl;
#endif
            PROTO proto_notifyEventSatisfy = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventSatisfy",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(u32), PIN_PARG_END());
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
            // *out << "replace notifyShutdown" << endl;
            //#endif
            // PROTO proto_notifyShutdown =
            // PROTO_Allocate(PIN_PARG(void), CALLINGSTD_DEFAULT,
            //"notifyShutdown", PIN_PARG_END());
            // RTN_ReplaceSignature(rtn, AFUNPTR(fini), IARG_PROTOTYPE,
            // proto_notifyShutdown, IARG_END);
            // PROTO_Free(proto_notifyShutdown);
            //}

            // replace notifyDBDestroy
            // rtn = RTN_FindByName(img, "notifyDbDestroy");
            // if (RTN_Valid(rtn)) {
            //#ifdef DEBUG
            // *out << "replace notifyDbDestroy" << endl;
            //#endif
            // PROTO proto_notifyDbDestroy = PROTO_Allocate(
            // PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyDbDestroy",
            // PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            // RTN_ReplaceSignature(rtn, AFUNPTR(afterDbDestroy),
            // IARG_PROTOTYPE, proto_notifyDbDestroy,
            // IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
            // PROTO_Free(proto_notifyDbDestroy);
            //}

            // replace notifyEventPropagate
            // rtn = RTN_FindByName(img, "notifyEventPropagate");
            // if (RTN_Valid(rtn)) {
            //#ifdef DEBUG
            //*out << "replace notifyEventPropagate" << endl;
            //#endif
            // PROTO proto_notifyEventPropagate = PROTO_Allocate(
            // PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventPropagate",
            // PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            // RTN_ReplaceSignature(rtn, AFUNPTR(afterEventPropagate),
            // IARG_PROTOTYPE, proto_notifyEventPropagate,
            // IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
            // PROTO_Free(proto_notifyEventPropagate);
            //}

#ifdef OUTPUT_CG
        // replace notifyEdtTerminate
        rtn = RTN_FindByName(img, "notifyEdtTerminate");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEdtTerminate";
            PROTO proto_notifyEdtTerminate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtTerminate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterEdtTerminate),
                                 IARG_PROTOTYPE, proto_notifyEdtTerminate,
                                 IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE,
                                 0, IARG_END);
            PROTO_Free(proto_notifyEdtTerminate);
#endif
        }
#endif
    }
}

int usage() {
    cout << "OCR-Racer is a graph traversal based data race detector for "
            "Open Community Runtime"
         << endl;
    cout << "Usage: pin -t [path to OCR-Racer.so] -- [path to OCR "
            "app][arg1][arg2]..."
         << endl;
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
        string arg = argv[argi];
        if (arg == "--") {
            break;
        }
    }
    if (argi == argc - 1 || argi == argc) {
        usage();
        exit(1);
    }
    userCodeImg = argv[argi + 1];
    *out << "User image is " << userCodeImg << endl;
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
             << endl;
        PIN_ExitProcess(1);
    }
    PIN_AddThreadStartFunction(threadStart, NULL);
    PIN_AddThreadFiniFunction(threadFini, NULL);
    PIN_AddFiniFunction(fini, 0);
    IMG_AddInstrumentFunction(overload, 0);
    //#ifdef INSTRUMENT
    // IMG_AddInstrumentFunction(instrumentImage, 0);
    //#endif

#ifdef MEASURE_TIME
    program_start = clock();
#endif
    PIN_StartProgram();
    return 0;
}
