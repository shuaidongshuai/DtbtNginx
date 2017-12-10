#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

/* 
多进程的锁 
注意：锁的内容需要自己mmap或者shmget创建到共享区（不然子进程会copy父进程的内容）
*/
class locker
{
public:
    locker() {
        pthread_mutexattr_init(&mutex_shared_attr);//初始化互斥对象属性
        //设置互斥对象为PTHREAD_PROCESS_SHARED共享，即可以在多个进程的线程访问,PTHREAD_PROCESS_PRIVATE为同一进程的线程共享 
        pthread_mutexattr_setpshared(&mutex_shared_attr,PTHREAD_PROCESS_SHARED);
        if( pthread_mutex_init( &mutex, &mutex_shared_attr ) != 0 )//2.ÊôÐÔ£¨NULL±íÊ¾Ä¬ÈÏ£©
        {
            throw std::exception();
        }
    }
    ~locker() {
        pthread_mutex_destroy( &mutex );
    }
    bool lock() {
        return pthread_mutex_lock( &mutex ) == 0;//Ô­×Ó²Ù×÷£¬½øÐÐ¼ÓËø,Èç¹ûÒÑ¾­¼ÓËø¾Í×èÈû
    }
	bool trylock() {
        return pthread_mutex_trylock( &mutex ) != EBUSY;//Ïà±ÈÓÚlock£¬trylockÊÇ·Ç×èÈûµÄ
    }
    bool unlock() {
        return pthread_mutex_unlock( &mutex ) == 0;//Ô­×Ó²Ù×÷£¬½âËø
    }
private:
    pthread_mutex_t mutex;
    pthread_mutexattr_t mutex_shared_attr;
};
#endif
