#include "head/executor.h"
#include "head/node.h"
#include "head/schedule.h"

namespace quiet_flow{

Graph::~Graph() {
}

void Graph::clear_graph(){
    _mutex.lock();
    for (auto i: nodes) {
        int cnt = 0;
        while (i->status != RunningStatus::Recoverable) {
            cnt ++;
            #ifdef QUIET_FLOW_DEBUG
            std::cout << (unsigned long int)i << "\n";
            #else
            if ((cnt % 8) == 0) {
                usleep(1);
            }
            #endif
        }
        i->_mutex.lock();   // 保证高并发下，set_status(Recoverable)
        i->_mutex.unlock();
        delete i;
    }
    _mutex.unlock();
    nodes.clear();
}

void Graph::get_nodes(std::vector<Node*>& required_nodes) {
    _mutex.lock();
    for (auto n_: nodes) {
        required_nodes.push_back(n_);
    } 
    _mutex.unlock();
}

Node* Graph::create_edges(Node* new_node, const std::vector<Node*>& required_nodes) {
    {
        _mutex.lock();
        nodes.push_back(new_node);
        _mutex.unlock();
    }

    if (required_nodes.size() > 0) {
        new_node->add_wait_count(required_nodes.size());

        for (auto r_node: required_nodes) {
            r_node->add_downstream(new_node);
        }
    } else {
        Schedule::add_new_task(new_node);
    }

    return new_node;
}

void Node::set_status(RunningStatus s) {
    _mutex.lock();
    status = s;
    _mutex.unlock();
}

int Node::add_downstream(Node* node) {
    if (this == node) {
        if (1 == node->sub_wait_count()) {
            Schedule::add_new_task(node);
        }
        return -1;
    }

    if (status >= RunningStatus::Finish) {
        if (1 == node->sub_wait_count()) {
            Schedule::add_new_task(node);
        }
        return -1;
    }
    
    _mutex.lock();
    if (status >= RunningStatus::Finish) {
        _mutex.unlock();
        if (1 == node->sub_wait_count()) {
            Schedule::add_new_task(node);
        }
        return -1;
    }
    down_streams.push_back(node);
    _mutex.unlock();

    return 0;
}

}