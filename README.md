# 1. 背景
## 1.1 为什么要处理DAG
最开心的代码是 main 函数写到底，输出各种 hello word。稍微复杂的逻辑，需要处理各种不同的 case。 再复杂的一点的业务就需要处理各种数据：数据就绪去做一些事情。只是为了效率，我们不能接受所有的事情都等着就绪去做。于是就有了类似于下面的依赖关系 DAG：

<img width="280" height="300" style="display:block; margin:0 auto;" src=https://user-images.githubusercontent.com/19359850/90396810-72f61680-e0c9-11ea-97eb-c3ca3c6fe858.png center />

我们希望上图各节点最大并的执行（当然必须正确）。所以，有些问题无法绕过：
1. 如何构建这个图？谁来构建这个图？
2. 如何低学习成本、低心智负担建图？图怎么避免循环依赖？
3. 哪些节点不需要浪费资源空等 （如 IO 未就绪）：阻塞非阻塞？无用浪费线程乃至引擎死机
4. 不去空等那如何知道数据就绪？

## 1.2 用户诉求是什么

我只想写业务，总之你帮我跑，别跑错！
- 同步异步我不关心，不要让我 then/callback，我只知道我的数据依赖
- 不要告诉我：建节点、建边（理论上还得防止出环）、graph.run、graph.wait ……

### 1.2.1 理想的用户界面

```
void func() {
    // 与 func 并发执行，早提交早执行
    task = sumbit_task(……)  
    do xxx
    do yyy
    // 等待 task 数据就绪：  最好 wait 不浪费执行资源
    wait_data(task)       
    do zzz
}
```

**扩展了对异步rpc的支持**
```
void func() {
    folloy::future f = async_rpc.xxx();
    do xxx
    do yyy
    // 等待 rpc 数据就绪
    wait_data(f)       
    do zzz
}
```

### 1.2.2 架构的承诺

作为引擎，我们需要保证什么

1. 执行引擎的线程尽量高效：不要 wait，我下游的线程尽量饷
2. 所有逻辑托管给引擎，业务要做的只是写一个 node, 同时声明依赖的 node
    1. 当前框架，创建图的线程其实是不受引擎控制的
    2. 不要复杂的判环逻辑，让我永远写不出环
  
## 1.3 技术的演变

### 1.3.1 堆线程池

最原始的实现，每个 request 开辟一个新线程（进程）。进一步，使用线程池让 request 复用线程。

但是，这种设计 request 的内部逻辑仍然是串行执行。

### 1.3.2 更高的并发 

演进上一步逻辑，每个请求拆分多个阶段。进而，每个阶段可以拆分出多个子请求，使用线程池最大限度并发。

进一步，如果某个节点存在 IO 相应的阻塞式逻辑，当前线程池需要空耗等待，乃至系统资源耗尽！扩大线程池？？

### 1.3.3 Callback 技术

参考 redis 单线程服务超高并发思路。使用 io 复用思路 + callback 唤醒：线程不去空等，而是数据就绪后再执行后续逻辑。注册 callback, 上游节点完成通知 or 执行后续逻辑。

比如 sogou/workflow
```
WFHttpTask *task = WFTaskFactory::create_http_task(url, REDIRECT_MAX, RETRY_MAX, wget_callback);
protocol::HttpRequest *req = task->get_req();
req->add_header_pair("Accept", "*/*");
req->add_header_pair("User-Agent", "Wget/1.14 (gnu-linux)");
req->add_header_pair("Connection", "close");
task->start();pause();
```

写起来更舒服的 folly::future
```
Future<double> fut = xxx(input).then(futureA).then(futureB)
    .then([](OutputD outputD) { 
        // 支持 lambda 表达式 
        return outputD * M_PI;
    });
```

相对于 callbck 的注册式，使用 then 语法把逻辑串起来，界面更加亲民，编码者心智负担更低。

**一些疑问**

这些 callback 执行在哪里？是 IO 的线程池吗？如果 callback 逻辑很重 IO 线程池会不会耗尽？IO 线程是不是只应该负责序列化、反序列化？

合理角度，callback 里面的业务逻辑，应该切回到 dag 引擎综合协调资源。

**成熟的封装**
- goroutine：使用 MPG 的概念协调资源与让度；“仿抢占”式调度及时出让资源，逻辑过重会卡死
- gevent：python 基于 epoll 的单线程协程机制（核心逻辑类似于 redis），逻辑过重会卡死
- libco：应该是仿 gevent 做的单线程设计 （未深入研究）

## 1.4 总结

用户方面
1. 用户以同步的思路写代码，不给编码者心智负担 OR 思路跳跃。

技术方面
1. 作为 c++ 框架，尽量不引入高级特性。如 c++20 的协程
2. 降低已有逻辑迁移成本，只作为 library 接入业务，做到渐近式接入
3. 作为调度引擎，应该托管所有核心资源（引擎线程池执行所有业务逻辑）

# 2. 关键特性

代码仓库，简要概括
- QuietFlow 避免用户显示写 callback，直接用 wait 方式等待数据
- QuietFlow 是一个流程编排系统
  1. 用户界面是核心考虑点，流式 coding 并发 working 
  2. 性能其次
  3. 不对标 goroutine/libco 解决普适问题（锁阻塞……）
- 使用协程的思想，保证线程满载，提高执行效率

## 2.1 两个用户侧概念

用户只需要关注两个概念
- 节点逻辑：我的节点业务逻辑是什么
- 节点依赖：我的节点依赖哪个节点提供的数据

用户不需要关注如何构建一个图，什么时候 `graph.run`、什么时候 `graph.start`。
1. 创建一个节点，如果节点没有依赖（依赖已经就绪），节点自动执行
2. 所有的逻辑都使用引擎的线程池执行，不需要（强烈不建议）用户自己维护线程池

用户不需要关心，什么是动态图、什么是静态图，什么是动态节点。也不需要关心当前阻塞 & 非阻塞!

## 2.2 协程思想 

可以认为是协程魔改，没有引入任何新 feature，如下代码：
```
void func() {
    task = sumbit_task(……)  // 与 func 并发执行
    do xxx
    do yyy
    wait_data(task)        // 等待 task 数据就绪：  最好 wait 不浪费执行资源
    do zzz
}
```

正常思路wait_data 处，需要 thread 阻塞，直到数据就绪。但是 wait 占用线程池资源，制约执行效率，甚至资源枯竭。

```
void wait_data(task) {
    // 类协程思路将执行切走
    swapcontext(make_old_context(task), make_new_context());
} 
void make_new_context() {
    1. 为执行创建一个新栈  malloc(8M)
    2. 后续执行切换到线程的消费入口，重新消费新 task 
}
void make_old_context(task) {
    1. 新加一个 task_back 依赖 task
    2. task 执行完，会触发 task_back 执行
    3. task_back 的核心逻辑就是将，old_context 切回 thread
}
```

## 2.3 如何防止依赖环

**1. 机制上避免子图内部环**

任务的提交通过  create_edges 创建新节点，同时构建依赖关系。创建新节点，直接声明依赖的数据节点（即：只能新节点依赖旧节点，永远不可能出环）。
```
// 插入任务
auto node_1 = sub_graph->create_edges(new NodeDemo(), {});       
// node_2 依赖 node_1
auto node_2 = sub_graph->create_edges(new NodeDemo(), {node_1});    
```

**2. 动态节点插入判断环**

**TODO**: 需要判断这种情况
```
static Node* outter_node;

class NodeDemo: public Node {
  public:
    void run() {
        auto sub_graph = get_graph();
        auto node_1 = sub_graph->create_edges(new NodeA(), {});
        // node_2 依赖 node_1
        auto node_2 = sub_graph->create_edges(new NodeB(), {node_1});

        // 新加入节点依赖当前节点 this
        auto node_error = sub_graph->create_edges(new NodeB(), {this});
        // 间接依赖 this，如 outer_node_xxx 依赖 this
        auto node_error_2 = sub_graph->create_edges(new NodeB(), {outer_node_xxx});
```

## 2.4 maybe more

# 3. 性能测试

1. 对照已有的静态图模式，测试 `fibonacci` 逻辑
  - A~M 计算节点分别计算 fibonacci(1) ~ fibonacci(13)
  - 图引擎使用10个 worker 线程
  - 20个压测线程构建 graph 提交: 每个压测线程运行10000次图然后退出
  - QuietFlow 10s 

<img width="280" height="300" style="display:block; margin:0 auto;" src=https://user-images.githubusercontent.com/19359850/90396810-72f61680-e0c9-11ea-97eb-c3ca3c6fe858.png center />

<img width="580" height="380" style="display:block; margin:0 auto;" src=https://user-images.githubusercontent.com/19359850/90396853-873a1380-e0c9-11ea-9f04-bf7ff08728e7.png center />


2. 动态图模式，测试  fibonacci 逻辑
  - A~M 计算节点分别计算 fibonacci(1) ~ fibonacci(13)
  - 压测线程直接提交  20*10000 张图，然后退出，引擎负责完成执行
  - QuietFlow 15s 需要创建 20万协程个协程栈，大量的 malloc | free
    - QuietFlow 对这种超大量纯 cpu 的节点，并不太适用于
    
<img width="580" height="230" style="display:block; margin:0 auto;" src=https://user-images.githubusercontent.com/19359850/90396833-7ee1d880-e0c9-11ea-9ebb-64447ba92902.png center />

# 4. 问题 && TODO
- 减少锁粒度 && 更多无锁化处理
- 节点支持 timeout 功能、安全降级功能、中断功能
- 每次切换需要切换栈，malloc(8M) 系统冲击较大： 内存池？动态大小栈？……
- worksteal ……
- maybe 协程级别的锁？IO 封装？  

时间原因代码性能尚有不少问题 & 改进点。不要纠结于实现，比如为什么用 posix 的 context 接口？为什么用了很多锁？为什么有些实现没用 thread local？

理解万岁，领会精神，代码分分钟可以切换走

# 5. 后记
个人认为好的框架、架构，应该是面向用户设计的。底层可以略矬，可能有某些奇技淫巧但是核心还是要先想用户需要怎么用。所以才有了 QuietFlow 相应的解决方案……

一些心得，高并发下任何小的顺序问题都可以被放大，而且能被放大出来的都是简单的问题。最恶心的事几十万并发下，还是万分之一的错误。 代码中注释着“不要乱动”的地方都是惨重的 debug 史。

QuietFlow 重点在解决用户界面问题，性能方面待优化。

