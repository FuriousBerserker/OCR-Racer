#ifndef __COMPUTATION_GRAPH__
#define __COMPUTATION_GRAPH__

#include <cstddef>
#include <map>
#include <string>
#include <iostream>
#include "ConcurrentHashMap.hpp"
#include "const_val.hpp"

class Node;
class Task;

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
    std::string toString() { return "[color=" + color + ", style=" + style + "]"; }
};
#endif

class Dep {
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
    void updateCache(uint64_t srcID, uint32_t srcEpoch, uint64_t dstID);
    void updateTaskFinalEpoch(uint64_t taskID, uint32_t epoch);
#ifdef OUTPUT_CG
    void toDot(std::ostream& outputStream);
#endif
   private:
#ifdef OUTPUT_CG
    std::map<Node::Type, ColorScheme> nodeColorSchemes;
    std::map<Node::EdgeType, ColorScheme> edgeColorSchemes;
    void outputLink(std::ostream& outputStream, Node* n1, uint32_t epoch1, Node* n2, uint32_t epoch2,
                    Node::EdgeType edgeType);
#endif
};


#endif
