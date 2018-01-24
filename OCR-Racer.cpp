
/*!next->parent->id == srcID @file
 *
 *  This is an example of the PIN tool that demonstrates some basic PIN APIs
 *  and could serve as the starting point for developing your first PIN tool
 */

#define __USE_PIN_CAS

#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "pin.H"
#include "ConcurrentHashMap.hpp"
#include "atomic/ops-enum.hpp"
#include "atomic/ops.hpp"
#include "ocr-types.h"
#include "const_val.hpp"
#include "computation_graph.hpp"
#include "ocr_race_util.hpp"

#ifdef MEASURE_TIME
clock_t program_start, program_end;
#endif



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

ComputationGraph computationGraph(10000);


inline bool isOCRLibrary(IMG img) {
    string ocrLibraryName = "libocr_x86.so";
    string imageName = IMG_Name(img);
    if (isEndWith(imageName, ocrLibraryName)) {
        return true;
    } else {
        return false;
    }
}

inline bool isUserCodeImg(IMG img) {
    string imageName = IMG_Name(img);
    if (imageName == userCodeImg) {
        return true;
    } else {
        return false;
    }
}

inline bool isIgnorableIns(INS ins) {
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
inline bool isNullGuid(ocrGuid_t guid) {
    if (guid.guid == NULL_GUID.guid) {
        return true;
    } else {
        return false;
    }
}


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
    //std::vector<DataBlockSM*> acquiredDB;
    //void initializeAcquiredDB(uint32_t dpec, ocrEdtDep_t* depv);
    //void insertDB(ocrGuid_t& guid);
    //DataBlockSM* getDB(uintptr_t addr);
    //void removeDB(DataBlock* dbPage);
//
    //private:
    //bool searchDB(uintptr_t addr, DataBlockSM** ptr, uint64_t* offset);
};



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
    assert(event != nullptr);
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

        // replace notifyEdtTerminate
        rtn = RTN_FindByName(img, "notifyEdtTerminate");
        if (RTN_Valid(rtn)) {
#ifdef DEBUG
            *out << "replace notifyEdtTerminate" << endl;
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

void recordMemRead(void* addr, uint32_t size, ADDRINT sp, ADDRINT ip) {
#ifdef DEBUG
    *out << "record memory read" << endl;
#endif

#ifdef DEBUG
    *out << "record memory read end" << endl;
#endif
}

void recordMemWrite(void* addr, uint32_t size, ADDRINT sp, ADDRINT ip) {
#ifdef DEBUG
    *out << "record memory write" << endl;
#endif

#ifdef DEBUG
    *out << "record memory write" << endl;
#endif
}

void instrumentInstruction(INS ins) {
    if (isIgnorableIns(ins)) return;

    if (INS_IsAtomicUpdate(ins)) return;

    uint32_t memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (uint32_t memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsRead(ins, memOp)) {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)recordMemRead,
                                     IARG_MEMORYOP_EA, memOp,
                                     IARG_MEMORYREAD_SIZE, IARG_REG_VALUE,
                                     REG_STACK_PTR, IARG_INST_PTR, IARG_END);
        }
        // Note that in some architectures a single memory operand can be
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp)) {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)recordMemWrite, IARG_MEMORYOP_EA,
                memOp, IARG_MEMORYWRITE_SIZE, IARG_REG_VALUE, REG_STACK_PTR,
                IARG_INST_PTR, IARG_END);
        }
    }
}

void instrumentRoutine(RTN rtn) {
    RTN_Open(rtn);
    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
        instrumentInstruction(ins);
    }
    RTN_Close(rtn);
}

void instrumentImage(IMG img, void* v) {
#ifdef DEBUG
    cout << "instrument image\n";
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
    cout << "instrument image finish\n";
#endif
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
#ifdef INSTRUMENT
    IMG_AddInstrumentFunction(instrumentImage, 0);
#endif

#ifdef MEASURE_TIME
    program_start = clock();
#endif
    PIN_StartProgram();
    return 0;
}
