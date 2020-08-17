#include <folly/futures/Future.h>

#include "head/executor.h"
#include "head/node.h"
#include "head/schedule.h"

namespace quiet_flow{

class BackNode: public Node {
  public:
    RunningStatus self_status;
    std::shared_ptr<ExecutorContext> back_run_context_ptr;
  public:
    BackNode(const std::string& debug_name="") {
        self_status = RunningStatus::Initing;
        #ifdef QUIET_FLOW_DEBUG
        name_for_debug = "back_node@" + debug_name;
        #endif
    }
    ~BackNode() {
        #ifdef QUIET_FLOW_DEBUG
        name_for_debug = "free_@" + name_for_debug;
        #endif
    }
    void run() {
        {
            _mutex.lock();
            status = RunningStatus::Recoverable;
            _mutex.unlock();
        }

        while (back_run_context_ptr->get_status() != RunningStatus::Yield) {
            usleep(1);
        }

        back_run_context_ptr->set_status(RunningStatus::Running);

        auto thread_exec = Schedule::unsafe_get_cur_thread();
        thread_exec->s_setcontext(back_run_context_ptr);
    }
};

class EndNode: public Node {
  public:
    EndNode(const std::string& debug_name="") {
        #ifdef QUIET_FLOW_DEBUG
        name_for_debug = "end_node@" + debug_name;
        #endif
    }
    ~EndNode() {
    }
    void run() {
        #ifdef QUIET_FLOW_DEBUG
        std::cout << name_for_debug << std::endl;
        #endif
    }
};

Node::Node() {
    #ifdef QUIET_FLOW_DEBUG
    name_for_debug = "node";
    #endif
    sub_graph = new Graph();
    status = RunningStatus::Initing;
    wait_count = 0;
}

void Node::require_node(const std::vector<Node*>& nodes, const std::string& sub_node_debug_name) {
    bool need_wait = false;
    for (auto& node: nodes) {
        if (node->status < RunningStatus::Finish) {
            need_wait = true;
        }
    }
    if (need_wait) {
        auto back_node = new BackNode(sub_node_debug_name);
        auto thread_exec = Schedule::unsafe_get_cur_thread();
        back_node->back_run_context_ptr = thread_exec->context_ptr;
        getcontext(&back_node->back_run_context_ptr->context);

        if (back_node->self_status == RunningStatus::Initing) {
            back_node->self_status = RunningStatus::Ready;

            sub_graph->create_edges(back_node, nodes);  // 这里可以提前触发 back_node, 进而破坏  stack
            status = RunningStatus::Yield; 
            std::shared_ptr<ExecutorContext> ptr = back_node->back_run_context_ptr;         // 这里不要删！！！
            thread_exec->swap_new_context(back_node->back_run_context_ptr, Schedule::jump_in_schedule);
        }
        back_node->back_run_context_ptr = nullptr;
        thread_exec = Schedule::unsafe_get_cur_thread();
        thread_exec->context_pre_ptr = nullptr;
        status = RunningStatus::Running; 
    }
}

void Node::require_node(folly::Future<int> &&future, const std::string& sub_node_debug_name) {
    auto end_node = new EndNode(sub_node_debug_name);
    std::move(future).onError(
        [end_node](int) mutable {end_node->status = RunningStatus::Fail;return 0;}
    ).then(
        [this, end_node](int) mutable {sub_graph->create_edges(end_node, {});}
    );
    require_node(std::vector<Node*>{end_node}, sub_node_debug_name);
}

void Node::wait_graph(Graph* graph, const std::string& sub_node_debug_name) {
    std::vector<Node*> required_nodes;
    graph->get_nodes(required_nodes);
    require_node(required_nodes, sub_node_debug_name);
}

void Node::resume() {
    assert(status == RunningStatus::Ready);

    {
        _mutex.lock();
        status = RunningStatus::Running;
        _mutex.unlock();
    }
    run();
    name_for_debug += "--end";
}

int Node::sub_wait_count() {
    return wait_count.fetch_sub(1, std::memory_order_relaxed);
}

int Node::add_wait_count(int upstream_count) {
    return wait_count.fetch_add(upstream_count, std::memory_order_relaxed);
}

void Node::finish(std::vector<Node*>& notified_nodes) {
    status = RunningStatus::Finish;

    _mutex.lock();
    notified_nodes.reserve(down_streams.size());
    for (auto d: down_streams) {
        if (1 == d->sub_wait_count()) {
            notified_nodes.push_back(d);
        }
    }
    _mutex.unlock();
}

class NodeRunWaiter: public Node {
  public:
    sem_t sem;
  public:
    NodeRunWaiter() {
        sem_init(&sem, 0, 0);
    }
    void run() {
        sem_post(&sem);
    }
};

void Node::block_thread_for_group(Graph* sub_graph) {
    // 会阻塞当前线程, 慎用

    std::vector<Node*> required_nodes;
    sub_graph->get_nodes(required_nodes);

    Graph g;
    NodeRunWaiter* waiter = new NodeRunWaiter();
    g.create_edges(waiter, required_nodes); // 插入任务

    sem_wait(&waiter->sem);
}
}