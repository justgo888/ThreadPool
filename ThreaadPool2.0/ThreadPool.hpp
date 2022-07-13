#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = 2;      // INT32_MAX
const int THREAD_MAX_THRESHHOLD = 1024; //最大线程数量
const int THREAD_MAX_IDLE_TIME = 60;    //线程最大空闲时间

/*线程池支持的模式*/
enum class PoolMode
{
    MODE_FIXED, //线程数量固定
    MODE_CACHED //线程数量可增长
};

/*
 *线程类对象
 *线程类在创建时会赋予一个编号，便于回收线程
 */
class Thread
{
public:
    //需要线程执行的函数的函数对象,void可以利用一个中间层获取返回值，int表示传入线程id
    using ThreadFunc = std::function<void(int)>;
    //构造函数,赋予线程编号
    Thread(ThreadFunc func)
        : func_(func), threadId_(generateId_++)
    {
        //为什么在此处不直接调用start函数启动线程呢？
        //是为了将线程对象的创建和线程的执行分开
    }
    //析构函数
    ~Thread() = default;
    //线程启动函数，创建一个线程用于执行指定的线程函数
    void start()
    {
        std::thread t(func_, threadId_); //创建一个线程用于执行线程函数
        t.detach();                      //分离线程，线程执行结束自动回收。所以析构函数不需要回收线程
    }
    //获取线程id
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;       //线程需要执行的函数的函数对象
    static int generateId_; //每生产一个线程对象就+1，静态成员变量需要类外声明
    int threadId_;          //由generateId_赋值,记录线程对象的编号
};
int Thread::generateId_ = 0;

/*
 *线程池对象
 */
class ThreadPool
{
public:
    //构造函数
    //初始化时可自定义设定线程池模式，初始线程数量，最大线程数量，最大任务数量
    ThreadPool(PoolMode poolmode = PoolMode::MODE_FIXED, int initthreadsize = std::thread::hardware_concurrency(), int maxthreadsize = THREAD_MAX_THRESHHOLD, int taskmaxsize = TASK_MAX_THRESHHOLD)
        : initThreadSize_(initthreadsize), maxThreadSize_(maxthreadsize), currentThreadNum_(0), idleThreadNum_(0), taskSize_(0), taskMaxSize_(taskmaxsize), poolmode_(poolmode), isRunning_(false)
    {
    }
    //析构函数
    ~ThreadPool()
    {
        isRunning_ = false;
        std::unique_lock<std::mutex> lock(taskMtx_); //对任务队列上锁，不允许再提交任务
        //等待线程池里面所有的线程返回(这些线程要么在执行任务，要么处于阻塞状态)
        notEmpty_.notify_all(); //通知消费任务，所有任务消费完了就会空闲线程回收
        //等到线程池所有的线程都回收了才结束阻塞状态
        exitCond_.wait(lock, [&]() -> bool
                       { return taskQue_.size() == 0; });
    }
    //通过可变参模板编程使得submittask可以接收任意任务函数和任意数量的参数
    //提交任务,通过future和自动类型推导获取返回值类型
    template <typename Func, typename... Args>
    auto submitTask(Func &&func, Args &&...args) -> std::future<decltype(func(args...))>
    {
        /*打包任务，放入任务队列中*/
        using RType = decltype(func(args...)); //定义返回值类型
        //利用forword和packaged_task打包任务
        auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        //获取锁
        std::unique_lock<std::mutex> lock(taskMtx_);
        //提交任务，如果提交超过一秒还不能提交成功，返回一个0值
        if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                               { return taskQue_.size() < taskMaxSize_; }))
        {
            //任务提交失败
            std::cerr << "submit task timeout!" << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>([]() -> RType
                                                                      { return RType(); });
            (*task)();
            return task->get_future();
        }
        //任务队有空余
        taskQue_.emplace([task]()
                         { (*task)(); });
        taskSize_++;

        //通知线程消费任务
        notEmpty_.notify_all();

        // cached模式下需要根据当前任务数量和空闲线程数量来判断是否需要创建新的线程来应对较多的任务
        if (poolmode_ == PoolMode::MODE_CACHED && taskQue_.size() > idleThreadNum_ && currentThreadNum_ < maxThreadSize_)
        {
            std::cout << ">>> create new thread..." << std::endl;

            //创建新的线程
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();                  //获取线程对象的编号
            threadMap_.emplace(threadId, std::move(ptr)); //线程对象加入管理表
            //启动线程
            threadMap_[threadId]->start();
            //修改线程个数相关的变量
            currentThreadNum_++;
            idleThreadNum_++;
        }

        return result;
    }
    //开启线程池
    void start()
    {
        isRunning_ = true;
        currentThreadNum_ = initThreadSize_;
        //创建线程对象
        for (int i = 0; i < initThreadSize_; i++)
        {
            //创建线程对象的同时，将线程函数传给对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threadMap_.emplace(threadId, std::move(ptr)); //线程对象加入管理表
        }
        //启动线程
        for (int i = 0; i < initThreadSize_; i++)
        {
            threadMap_[i]->start(); //需要去执行线程函数
            idleThreadNum_++;
        }
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool) = delete;

private:
    //查询线程池运行状态
    bool checkState() const
    {
        return isRunning_;
    }
    //定义线程函数
    void threadFunc(int threadId)
    {
        //记录一下时间
        auto lastTime = std::chrono::high_resolution_clock().now();

        //所有任务都执行完成线程池才能回收所有线程
        for (;;)
        {
            Task task;
            {
                //获取锁
                std::unique_lock<std::mutex> lock(taskMtx_);

                std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

                // cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程结束回收掉（超过initThreadSize_数量的线程要进行回收）
                // 回收条件：当前时间 - 上一次线程执行的时间 > 60s
                //无论是线程池退出还是线程空余，要想结束线程的前提都必须是任务队列为空
                while (taskQue_.size() == 0)
                {
                    if (!isRunning_) //线程池的析构函数首先会将状态置为false
                    {
                        //线程池要被析构，因为线程函数是由各个线程执行，所以由当前线程回收当前线程
                        threadMap_.erase(threadId); //删除时Thread自动析构
                        std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                        exitCond_.notify_all(); //每个线程退出都通知一下，直到最后一个线程退出通知从而达到线程池析构
                        return;                 //线程函数结束，线程结束
                    }

                    if (poolmode_ == PoolMode::MODE_CACHED)
                    {
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            //如果还有任务提交，就会唤醒阻塞，那么此处就不会执行
                            //如果超时，就判断空闲时间是否达到要求
                            auto nowTime = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(nowTime - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && currentThreadNum_ > initThreadSize_)
                            {
                                threadMap_.erase(threadId);
                                currentThreadNum_--;
                                idleThreadNum_--;

                                std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                                return;
                            }
                        }
                    }
                    else
                    {
                        notEmpty_.wait(lock);
                    }
                }
                idleThreadNum_--;

                std::cout << "tid:" << std::this_thread::get_id()
                          << "获取任务成功..." << std::endl;

                // 从任务队列中取一个任务出来
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;

                // 如果依然有剩余任务，继续通知其它得线程执行任务
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

                // 取出一个任务，进行通知，通知可以继续提交生产任务
                notFull_.notify_all();
            } // 释放锁

            // 当前线程负责执行这个任务
            if (task != nullptr)
            {
                task(); // 执行function<void()>
            }

            idleThreadNum_++;
            lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
        }
    }

private:
    /*线程相关参数*/
    std::unordered_map<int, std::unique_ptr<Thread>> threadMap_; //线程队列
    int initThreadSize_;                                         //初始线程数量
    int maxThreadSize_;                                          //最大线程数量
    std::atomic_int currentThreadNum_;                           //当前线程池中线程总数量
    std::atomic_int idleThreadNum_;                              //当前线程池中空闲线程数量

    /*任务相关参数*/
    using Task = std::function<void()>;
    std::queue<Task> taskQue_;         //任务队列
    std::atomic_int taskSize_;         //任务数量
    int taskMaxSize_;                  //最大任务数量
    std::mutex taskMtx_;               //互斥锁，保证任务队列的线程安全
    std::condition_variable notEmpty_; //条件变量，表示任务队列不空
    std::condition_variable notFull_;  //条件变量，表示任务队列不满
    std::condition_variable exitCond_; //等到线程资源全部回收

    PoolMode poolmode_;          //线程池模式
    std::atomic_bool isRunning_; //线程池运行状态
};

#endif