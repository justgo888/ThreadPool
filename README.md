# ThreadPool
基于C++17标准的线程池
1.0版本自己实现了可接受任意类型的Any类以及可以根据用户提交任务的类型而进行返回值返回的Result类
2.0版本直接使用future和packaged_task机制重构线程池，为最终版