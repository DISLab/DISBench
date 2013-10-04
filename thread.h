#ifndef _thread_h_
#define _thread_h_

#include <pthread.h>

class Thread
{
private:
	pthread_t thread;
	static void * thread_func(void *d) { ((Thread *)d)->run(); return NULL; }
	
public:
	Thread() {}
	virtual	~Thread();

	virtual void run() = 0;

	int start() { 
		return pthread_create (&thread, NULL, 
				Thread::thread_func, (void *)this); 
	}
	int wait() { 
		return pthread_join (thread, NULL); 
	}

	virtual void print_report() = 0;
};

Thread::~Thread(){};

#endif
