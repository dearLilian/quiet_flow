#pragma once

#include <atomic>
#include <mutex>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "util.h"

namespace folly{template<class T>class Future;}

namespace quiet_flow{
  
class Node;

class Graph {
  public:
    Graph(){}
    ~Graph();
    void clear_graph();
    void get_nodes(std::vector<Node*>& required_nodes);
    Node* create_edges(Node* new_node, const std::vector<Node*>& required_nodes); 
  private:
    size_t idx;
    std::vector<Node*> nodes;
    std::mutex _mutex;
};

class Node {
  public:
    std::string name_for_debug;
  public:
    Node();
    virtual ~Node() {sub_graph->clear_graph(); delete sub_graph;}
    void resume();
    virtual void run() = 0;
    void finish(std::vector<Node*>& notified_nodes);
    void set_status(RunningStatus);
    RunningStatus unsafe_get_status() {return status;}
    void block_thread_for_group(Graph* sub_graph); // 会阻塞当前线程, 慎用
  protected:
    std::mutex _mutex;
    RunningStatus status;
  protected:
    void require_node(const std::vector<Node*>& nodes, const std::string& sub_node_debug_name="");
    void require_node(folly::Future<int> &&future, const std::string& sub_node_debug_name="");
    void wait_graph(Graph* graph, const std::string& sub_node_debug_name="");
    Graph* get_graph() {return sub_graph;}

  private:
    Graph* sub_graph;
    std::vector<Node*> down_streams;
    std::atomic<int> wait_count;

  friend class Graph;
  private:
    int add_wait_count(int upstream_count);
    int add_downstream(Node* node);
    int sub_wait_count();
};

}