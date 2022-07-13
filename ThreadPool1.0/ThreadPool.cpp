#include "ThreadPool.h"

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒

/////////////////////////////////////////////////////////////////////////////////////
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : isValid_(isValid), task_(task)
{
    task_->setResult(this);
}

//获取任务执行完的返回值的
void Result::setVal(Any any)
{
    this->any_ = std::move(any);
    sem_.post();
}
//用户通过get获取返回值
Any Result::get()
{
    if (isValid_)
    {
        return "";
    }
    sem_.wait();
    return std::move(any_);
}

////////////////////////////////////////////////////////////////////////////////
// Task类成员方法
Task::Task()
    : result_(nullptr)
{
}

void Task::exec()
{
    if (result_ != nullptr)
    {
        result_->setVal(run());
    }
}
void Task::setResult(Result *res)
{
    result_ = res;
}

//////////////////////////////////////////////////////////////////////////////////
//线程类成员方法定义
int Thread::generateId_ = 0; //静态成员变量声明

/*
 *线程构造函数
 *构造函数的功能:创建线程并启动执行线程函数
 */
Thread::Thread(ThreadFunc func)
    : func_(func), threadId_(generateId_++)
{
}

/*线程析构*/
Thread::~Thread()
{
}

/*启动线程*/
void Thread::start()
{
    //创建一个线程来执行一个线程函数 底层调用pthread_create
    std::thread t(func_, threadId_);
    t.detach(); //设置分离线程 pthread_detach pthread_t 设置为分离线程
}

/*获取线程id*/
int Thread::getId() const
{
    return threadId_;
}

//////////////////////////////////////////////////////////////////////////////////////////////
//线程池类成员方法
//线程池构造
ThreadPool::ThreadPool(int initThreadSize, int threadthreshhold, int taskthreshhold, PoolMode mode)
    : initThreadSize_(initThreadSize), threadSizeThreshHold_(threadthreshhold), curThreadNum_(0), idleThreadNum_(0), taskNum_(0), taskQueMaxThreshHold_(taskthreshhold), poolMode_(mode)
{
}
//线程池析构
ThreadPool::~ThreadPool()
{
    isPoolRunning_=false;
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]() -> bool
                   { return threadMap_.size() == 0; });
}
//给线程池提交任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    if (!notEmpty_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                            { return taskQue_.size() < taskQueMaxThreshHold_; }))
    {
        std::cerr << "task queue is full,please wait a moment" << std::endl;
        return Result(sp, false);
    }

    taskQue_.emplace(sp);
    taskNum_++;

    notEmpty_.notify_all();

    if (poolMode_ == PoolMode::MODE_CACHED && curThreadNum_ < threadSizeThreshHold_ && taskNum_ > idleThreadNum_)
    {
        std::cout << ">>> create new thread..." << std::endl;

        //创建新的线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));

        int threadId = ptr->getId();

        threadMap_.emplace(threadId, std::move(ptr));

        threadMap_[threadId]->start();

        curThreadNum_++;
        idleThreadNum_++;
    }

    return Result(sp);
}
//开启线程池
void ThreadPool::start()
{
    isPoolRunning_ = true;
    for (int i = 0; i < initThreadSize_; i++)
    {
        //创建新的线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));

        int threadId = ptr->getId();

        threadMap_.emplace(threadId, std::move(ptr));
    }

    for (int i = 0; i < initThreadSize_; i++)
    {
        threadMap_[i]->start();

        curThreadNum_++;
        idleThreadNum_++;
    }
}
//定义线程函数
void ThreadPool::threadFunc(int threadid)
{
    auto lastTime = std::chrono::high_resolution_clock().now();

    // 所有任务必须执行完成，线程池才可以回收所有线程资源
    for (;;)
    {
        std::shared_ptr<Task> task;
        {
            // 先获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            std::cout << "tid:" << std::this_thread::get_id()
                      << "尝试获取任务..." << std::endl;

            // cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程
            // 结束回收掉（超过initThreadSize_数量的线程要进行回收）
            // 当前时间 - 上一次线程执行的时间 > 60s

            // 每一秒中返回一次   怎么区分：超时返回？还是有任务待执行返回
            // 锁 + 双重判断
            while (taskQue_.size() == 0)
            {
                // 线程池要结束，回收线程资源
                if (!isPoolRunning_)
                {
                    threadMap_.erase(threadid); // std::this_thread::getid()
                    std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
                              << std::endl;
                    exitCond_.notify_all();
                    return; // 线程函数结束，线程结束
                }

                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    // 条件变量，超时返回了
                    if (std::cv_status::timeout ==
                        notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadNum_ > initThreadSize_)
                        {
                            // 开始回收当前线程
                            // 记录线程数量的相关变量的值修改
                            // 把线程对象从线程列表容器中删除   没有办法 threadFunc《=》thread对象
                            // threadid => thread对象 => 删除
                            threadMap_.erase(threadid); // std::this_thread::getid()
                            curThreadNum_--;
                            idleThreadNum_--;

                            std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
                                      << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    // 等待notEmpty条件
                    notEmpty_.wait(lock);
                }
            }

            idleThreadNum_--;

            std::cout << "tid:" << std::this_thread::get_id()
                      << "获取任务成功..." << std::endl;

            // 从任务队列种取一个任务出来
            task = taskQue_.front();
            taskQue_.pop();
            taskNum_--;

            // 如果依然有剩余任务，继续通知其它得线程执行任务
            if (taskQue_.size() > 0)
            {
                notEmpty_.notify_all();
            }

            // 取出一个任务，进行通知，通知可以继续提交生产任务
            notFull_.notify_all();
        } // 就应该把锁释放掉

        // 当前线程负责执行这个任务
        if (task != nullptr)
        {
            // task->run(); // 执行任务；把任务的返回值setVal方法给到Result
            task->exec();
        }

        idleThreadNum_++;
        lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
    }
}
