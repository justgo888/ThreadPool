#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <thread>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
/////////////////////////////////////////////////////////////////////////////////////////////////////
//返回值类型
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any &) = delete;
    Any &operator=(const Any &) = delete;
    Any(Any &&) = default;
    Any &operator=(Any &&) = default;

    //这个构造函数可以让Any类型接收任意其他的数据
    template <typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data))
    {
    }

    //去除data_存放的数据
    template <typename T>
    T cast()
    {
        Derive<T> *pd = dynamic_cast<Derive<T> *>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch";
        }
        return pd->data_;
    }

private:
    //基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    //派生类类型
    template <typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data) {}
        T data_; //保存了任意类型
    };

private:
    //定义一个基类的指针
    std::unique_ptr<Base> base_;
};

class Semaphore
{
public:
    Semaphore(int limit = 0) : resLimit_(limit) {}
    ~Semaphore() = default;

    //获取一个信号量资源
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_.wait(lock, [&]() -> bool
                   { return resLimit_ > 0; });
        resLimit_--;
    }
    //归还一个信号量资源
    void post()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }

private:
    int resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

class Task;

class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    //获取任务执行完的返回值的
    void setVal(Any any);
    //用户通过get获取返回值
    Any get();

private:
    Any any_; //存储任务的返回值
    Semaphore sem_;
    std::shared_ptr<Task> task_;
    std::atomic_bool isValid_;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////
//任务类型
class Task
{
public:
    Task();
    ~Task() = default;
    void exec();
    void setResult(Result *res);

    virtual Any run() = 0;

private:
    Result *result_;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////
//线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,  // fixed模式，线程数量固定
    MODE_CACHED, // cached模式，线程数量动态增长
};

//线程类型
class Thread
{
public:
    /*
     *线程需要执行的函数的函数对象
     *执行时需要传入线程对象构造时赋予的线程编号，所以参数为int类型
     */
    using ThreadFunc = std::function<void(int)>;

    /*
     *线程构造函数
     *构造函数的功能:创建线程并启动执行线程函数
     */
    Thread(ThreadFunc func);

    /*线程析构*/
    ~Thread();

    /*启动线程*/
    void start();

    /*获取线程id*/
    int getId() const;

private:
    ThreadFunc func_;       //存放传入的线程函数的函数对象
    static int generateId_; //线程编号赋值，使用静态可以保证线程安全
    int threadId_;          // generateId_赋值后保存在这里
};

///////////////////////////////////////////////////////////////////////////////////////////////////////线程池类型
class ThreadPool
{
public:
    //线程池构造
    ThreadPool(int initThreadSize = std::thread::hardware_concurrency(), int threadthreshhold = 1024,
               int taskthreshhold = 4, PoolMode mode = PoolMode::MODE_FIXED);
    //线程池析构
    ~ThreadPool();
    //给线程池提交任务
    Result submitTask(std::shared_ptr<Task> sp);
    //开启线程池
    void start();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

private:
    //定义线程函数
    void threadFunc(int threadid);

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threadMap_; //线程列表

    int initThreadSize_;             //初始线程数量
    int threadSizeThreshHold_;      //线程数量上限阈值
    std::atomic_int curThreadNum_;  //记录当前线程池中线程的总数量
    std::atomic_int idleThreadNum_; //记录空闲线程数量

    std::queue<std::shared_ptr<Task>> taskQue_; //任务队列
    std::atomic_int taskNum_;                   //任务的数量
    int taskQueMaxThreshHold_;                   //任务队列数量上限阈值

    std::mutex taskQueMtx_;            //保证任务队列的线程安全
    std::condition_variable notFull_;  //表示任务队列不满
    std::condition_variable notEmpty_; //表示任务队列不空
    std::condition_variable exitCond_; //等待线程资源全部回收

    PoolMode poolMode_; //当前线程池的工作模式
    std::atomic_bool isPoolRunning_;
};

#endif