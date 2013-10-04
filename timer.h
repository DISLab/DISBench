#ifndef _timer_h_
#define _timer_h_
#include <sys/time.h>

class Timer 
{
private:
	struct timeval tstart, tfinish;

public:
	Timer() {}
	~Timer() {}
	inline void begin() { gettimeofday(&tstart,NULL); }
	inline void end() { gettimeofday(&tfinish,NULL); }
	inline double gettime() 
	{ 
		return((double)(tfinish.tv_sec-tstart.tv_sec) +
			           ((double)(tfinish.tv_usec-tstart.tv_usec)/1000000.0));
	}
};

#endif
