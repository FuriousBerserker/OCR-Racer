#include <queue>
#include "ocr_race_util.hpp"
#include "computation_graph.hpp"

extern std::ostream* out;
extern std::ostream* err;

Node::Node(uint64_t id, Type type, Task* parent, uint32_t parentEpoch)
    : incomingEdges(10),
      parent(parent),
      parentEpoch(parentEpoch),
      type(type),
      id(id) {}

Node::~Node() {}

void Node::addDeps(uint64_t id, Dep& dep) {
    Dep* copy = new Dep(dep);
    incomingEdges.put(id, copy);
    //*out << id << "#" << dep.epoch << "->" << this->id << std::endl;
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

void ComputationGraph::insert(uint64_t key, Node* value) {
    nodeMap.put(key, value);
}

Node* ComputationGraph::getNode(uint64_t key) {
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
    std::queue<Node*> q;
    q.push(dstNode);
    while (!q.empty()) {
        Node* next = q.front();
        q.pop();
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
                next->parent->parentEpoch >= srcEpoch) {
                result = true;
                break;
            }
            if (next->parent && next->parent->id != srcID) {
                q.push(next->parent);
            }
            for (auto it = next->incomingEdges.begin(),
                      ie = next->incomingEdges.end();
                 it != ie; ++it) {
                Node* ancestor = (*it).second->src;
                if (ancestor->id == srcID) {
                    continue;
                }
                while (ancestor->type == Node::EVENT &&
                       ancestor->incomingEdges.getSize() == 1) {
                    auto it = ancestor->incomingEdges.begin();
                    ancestor = (*it).second->src;
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
                                   uint64_t dstID) {
    uint64_t cacheKey = generateCacheKey(srcID, dstID);
    if (cacheMap.get(cacheKey) < srcEpoch) {
        cacheMap.put(cacheKey, srcEpoch);
    }
}

void ComputationGraph::updateTaskFinalEpoch(uint64_t taskID,
                                                   uint32_t epoch) {
    epochMap.put(taskID, epoch);
}

#ifdef OUTPUT_CG
void ComputationGraph::toDot(std::ostream& outputStream) {
#ifdef DEBUG
    (*out) << "CG2Dot" << std::endl;
#endif
    (*out) << "node num = " << nodeMap.getSize() << std::endl;
    // used to filter redundant nodes, since we may point multiple ID to a
    // single node, for instance, we point an output event's ID to the
    // associated task  std::set<Node*> accessedNode;
    outputStream << "digraph ComputationGraph {" << std::endl;
    for (auto ci = nodeMap.begin(), ce = nodeMap.end(); ci != ce; ++ci) {
        Node* node = (*ci).second;
        // if (accessedNode.find(node) == accessedNode.end()) {
        // accessedNode.insert(node);
        //} else {
        // continue;
        //}
        std::string nodeColor = nodeColorSchemes.find(node->type)->second.toString();
        if (node->type == Node::TASK) {
            uint32_t finalEpoch = epochMap.get(node->id);
            for (uint32_t i = START_EPOCH + 1; i <= finalEpoch; i++) {
                outputStream << '\"' << node->id << "#" << i << '\"' << nodeColor << ";"
                    << std::endl;
            }
        } else {
            outputStream << '\"' << node->id << '\"' << nodeColor << ";" << std::endl;
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
                outputLink(outputStream, node->parent, node->parentEpoch, node,
                           START_EPOCH + 1, Node::SPAWN);
            }
            for (auto di = node->incomingEdges.begin(),
                      de = node->incomingEdges.end();
                 di != de; ++di) {
                Dep* dep = (*di).second;
                outputLink(outputStream, dep->src,
                           (dep->epoch == END_EPOCH ? epochMap.get(dep->src->id)
                                                    : dep->epoch),
                           node, START_EPOCH + 1, Node::JOIN);
            }
            uint32_t finalEpoch = epochMap.get(node->id);
            for (uint32_t i = START_EPOCH + 1; i < finalEpoch; i++) {
                outputLink(outputStream, node, i, node, i + 1, Node::CONTINUE);
            }
        } else {
            for (auto di = node->incomingEdges.begin(),
                      de = node->incomingEdges.end();
                 di != de; ++di) {
                Dep* dep = (*di).second;
                outputLink(outputStream, dep->src,
                           (dep->epoch == END_EPOCH ? epochMap.get(dep->src->id)
                                                    : dep->epoch),
                           node, START_EPOCH + 1, Node::JOIN);
            }
        }
    }
    outputStream << "}";
}

void ComputationGraph::outputLink(std::ostream& outputStream, Node* n1, uint32_t epoch1, Node* n2, uint32_t epoch2, Node::EdgeType edgeType) {
    outputStream << '\"' << n1->id;
    if (n1->type == Node::TASK) {
        outputStream << '#' << epoch1;
    }
    outputStream << '\"';
    outputStream << " -> ";
    outputStream << '\"' << n2->id;
    if (n2->type == Node::TASK) {
        outputStream << '#' << epoch2;
    }
    outputStream << '\"';
    outputStream << ' ' << edgeColorSchemes.find(edgeType)->second.toString();
    outputStream << ';' << std::endl;
}
#endif
