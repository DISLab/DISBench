//-----------------------------------------------------------------------------
// DISbench v0.1
// DISbench is a benchmark set targeted for evaluation of memory performanve
// on data intensive loads.
//-----------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <timer.h>
#include <thread.h>
#include <iostream>
#include <utmpx.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Global macros and constants
//-----------------------------------------------------------------------------
// Maximal CPU cores
#define MAX_CPU_CORES 	240

// Page sizes
#define PAGE_SIZE 			(4*1024)	
#define HUGE_PAGE_SIZE		(2*1024*1024)
#define GIGANTIC_PAGE_SIZE	(1024*1024*1024)

// Version.Revision
#define VERSION 		0
#define REVISION 		1

//-----------------------------------------------------------------------------
// Global types 
//-----------------------------------------------------------------------------
// Use this type if you want to test 32 or 64 bit architectures
typedef uint64_t word_t;

//-----------------------------------------------------------------------------
// Parameters & global variables
//-----------------------------------------------------------------------------
// test type
typedef enum {Stream=0, Stride, Random, Unknown} Testtype;
Testtype testtype = Stream;

const char *testnames[] = {"stream", "stride", "random"};
#define TESTNAME testnames[testtype]

// operation type
typedef enum {Load=0, Store, LoadModifyStore} Optype;
Optype optype = Load;

const char *optype_names[] = {"load", "store", "load-modify-store"};
#define OPTYPENAME optype_names[optype]

// Memory used for testing
word_t *memory = NULL;

// Memory size range (--mem, -m) in words (word_t)
unsigned long long Nmin=0,Nmax=0,Nstep=0;
unsigned long long N;

// Stride size (--stride, -s) in words (word_t)
unsigned long long Smin=0,Smax=0,Sstep=0;
unsigned long long S;

// Iteration number
unsigned long long K = 1024*1024*1024;

// Configuration flags
bool shared_memory = false;
bool use_index = false;
bool vect_loop = false;
int unroll_depth = 1;
unsigned long long start_offset = 0;

// Thread count and thread affinity
int thread_count=1;
int thread_affinity[MAX_CPU_CORES];

// NUMA support
#define MAX_NUMA_NODES 32 
int numa_nodes_count=1;
int numa_nodes[MAX_NUMA_NODES];
typedef enum {Blocked=0, Interleaved, OldPolicy} NumaPolicies;
NumaPolicies numa_policy = OldPolicy;
unsigned long long  numa_block_size = 1024;
const char *numa_policies[] = {"blocked", "interleaved", "old policy"};

//#define PAPI_USED
#ifdef PAPI_USED
#pragma message "PAPI enabled"
//-----------------------------------------------------------------------------
// PAPI support
//-----------------------------------------------------------------------------
#include <papi.h>

// PAPI event counter, events, values, etc.
int papi_counter = 0;
int papi_events[16];
long long papi_values[16];
char errorstring[PAPI_MAX_STR_LEN+1];

void papi_get_event_name(int event, char *name)
{
	switch(event)
	{
		case PAPI_L1_TCM: sprintf(name,"l1 misses"); break;
		case PAPI_L2_TCM: sprintf(name,"l2 misses"); break;
		case PAPI_L3_TCM: sprintf(name,"l3 misses"); break;
		case PAPI_TLB_DM: sprintf(name,"tlb misses"); break;
		default:
		  break;
	}
}
#endif

//-----------------------------------------------------------------------------
// DISThread is a base class for StreamThread, RandomThread, StrideThread
//-----------------------------------------------------------------------------
class DISThread : public Thread
{
public:
	int threadid;
    pid_t pid, tid;
    int used_cpu[MAX_CPU_CORES];
    int cpu_affinity;

    word_t * memory;
    unsigned long *index;
    Timer timer;

#ifdef PAPI_USED
	int papi_eventset;
#endif

	// use this variable as a optimization killer
	word_t devnull;

public:
    DISThread(int threadid, word_t *memory, int cpu_affinity) : Thread(),
        threadid(threadid), cpu_affinity(cpu_affinity), memory(memory)
    {
        // zero used_cpu
        for (int i = 0; i < MAX_CPU_CORES; i++)
            used_cpu[i] = 0;

    }
	~DISThread() {}

    void print_report()
    {
		char threadname[10];
		sprintf(threadname,"thread %d",threadid);

        printf("[%s]:PID %d, TID %d\n",threadname, pid, tid);
        printf("[%s]:used CPUs:",threadname);
        for (int i = 0; i < MAX_CPU_CORES; i++)
            if (used_cpu[i]) printf(" %d",i);
        printf("\n");
        printf("[%s]:Bandwidth %.2f MW/s (%.2f MB/s)\n", 
		   threadname, 
		   (double)(K)/(timer.gettime()*1024*1024),
		   (double)(K*8)/(timer.gettime()*1024*1024));

#ifdef PAPI_USED
        printf("[%s]:PAPI statistics: ",threadname);
        for (int i = 0; i < papi_counter; i++)
            switch(papi_events[i]) {
            case PAPI_TLB_DM:
                printf("TLB misses %lld (%.2f) ",
                       papi_values[i], (float)papi_values[i]/(K));
                break;
            case PAPI_L1_TCM:
                printf("L1 misses %lld (%.2f) ",
                       papi_values[i], (float)papi_values[i]/(K));
                break;
            case PAPI_L2_TCM:
                printf("L2 misses %lld (%.2f) ",
                       papi_values[i], (float)papi_values[i]/(K));
                break;
            case PAPI_L3_TCM:
                printf("L3 misses %lld (%.2f) ",
                       papi_values[i], (float)papi_values[i]/(K));
                break;
            default:
                printf("unexpected papi event");
                break;
            }
#endif
        printf("\n");
    }

	void profile_start()
	{
#ifdef PAPI_USED
		if (papi_counter > 0) {

			int retval;
			papi_eventset = PAPI_NULL;

			if ((retval = PAPI_register_thread()) != PAPI_OK){
				PAPI_perror(errorstring);//FIXME: add support for different PAPI versions
				//PAPI_perror(retval, errorstring, PAPI_MAX_STR_LEN);
				fprintf(stderr, "PAPI error (%d): %s\n", retval, errorstring);
				exit(1);
			}

			// create event set
			if ((retval = PAPI_create_eventset(&papi_eventset)) != PAPI_OK){
				PAPI_perror(errorstring);
				//PAPI_perror(retval, errorstring, PAPI_MAX_STR_LEN);
				fprintf(stderr, "PAPI error (%d): %s\n", retval, errorstring);
				exit(1);
			}

			// add papi events
			if ((retval = PAPI_add_events(papi_eventset, papi_events, papi_counter)) != PAPI_OK){
				PAPI_perror(errorstring);
				//PAPI_perror(retval, errorstring, PAPI_MAX_STR_LEN);
				fprintf(stderr, "PAPI error (%d): %s\n", retval, errorstring);
				exit(1);
			}

			/* Start counting */
			if (PAPI_start(papi_eventset) != PAPI_OK){
				fprintf(stderr, "PAPI error!\n");
				exit(1);
			}
		}
#endif
	}

	void profile_stop()
	{
#ifdef PAPI_USED
		if (papi_counter > 0) {

			int retval;
			if (PAPI_stop(papi_eventset,papi_values) != PAPI_OK){
				fprintf(stderr, "PAPI error!\n");
				exit(1);
			}
			if ((retval = PAPI_unregister_thread()) != PAPI_OK) {
				PAPI_perror(errorstring);
				//PAPI_perror(retval, errorstring, PAPI_MAX_STR_LEN);
				fprintf(stderr, "PAPI error (%d): %s\n", retval, errorstring);
				exit(1);
			}
		}
#endif
	}

    void run()
    {
        pid = getpid();
        tid = syscall(SYS_gettid);

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_affinity,&cpuset);
        pthread_setaffinity_np(pthread_self(),sizeof(cpu_set_t), &cpuset);

        used_cpu[sched_getcpu()] = 1;

        // if shared_memory is not set then all memory is private for cores
        if (!shared_memory)
        {
            memory = new word_t [Nmax + 16];

            // allign memory to cache line
            //memory = ((uint64_t)memory & ~(1<<6-1)) + 16;
			unsigned long boundN = N;
            for (unsigned long i = 0; i < boundN; i+=(PAGE_SIZE/sizeof(word_t)))
            {
                memory[i] = 1;
            }
        }

		profile_start();

        timer.begin();

		runtest();

        timer.end();

		profile_stop();

		//printf("sum = %lld\n",(long long)sum);

		if (!shared_memory)
			delete[] memory;
    }

	virtual void runtest() = 0;

	void foo(word_t sum) { printf("sum = %llu\n", (unsigned long long)sum); }
};
//-----------------------------------------------------------------------------
// StreamThread implements thread for stream access
//-----------------------------------------------------------------------------
class StreamThread : public DISThread
{
private:

public:
    StreamThread(int threadid, word_t * memory, int cpu_affinity) : 
		DISThread(threadid, memory, cpu_affinity) {}
	~StreamThread() {}

    void runtest()
    {
		word_t sum=0;
		word_t sum0=0,sum1=0,sum2=0,sum3=0,sum4=0,sum5=0,sum6=0,sum7=0;
		word_t sum8=0,sum9=0,sum10=0,sum11=0,sum12=0,sum13=0,sum14=0,sum15=0;
		//word_t *sum_dum = 0;

		unsigned int block_size = N/thread_count;
		unsigned int block_start = threadid*block_size;

		switch (optype) {
			case Load:
  				//#pragma vector aligned
  				if(vect_loop) {
  					//for (unsigned int i = 0/*, j = 0*/; i < bound; i++/*,j++*/) 
					/*for (unsigned int i = 0, j = block_start; i < K; i++,j=block_start + (j+1)%(block_size)) //FIXME: adjust boundaries and j range
  					{
  						sum += memory[j];
  					}*/

					printf("vectorized stream\n");

					for (unsigned int k = 0; k < K/block_size; k++) 
						for (unsigned int i = 0, j = block_start; i < block_size; i++, j++) //FIXME: adjust boundaries and j range
						{
							sum += memory[j];
						}
				}
  				else
					for (unsigned int i = 0, j = block_start; i < K/16; i++,j=block_start + (j+16)%(block_size)) //FIXME: adjust boundaries and j range
  					{
  						sum0 += memory[j];
  						sum1 += memory[j+1];
  						sum2 += memory[j+2];
  						sum3 += memory[j+3];
  						sum4 += memory[j+4];
  						sum5 += memory[j+5];
  						sum6 += memory[j+6];
  						sum7 += memory[j+7];
  	
  						sum = (sum0 + sum1 + sum2 + sum3 + sum4 + sum5 + sum6 + sum7);
  	
  						sum8 += memory[j+8];
  						sum9 += memory[j+9];
  						sum10 += memory[j+10];
  						sum11 += memory[j+11];
  						sum12 += memory[j+12];
  						sum13 += memory[j+13];
  						sum14 += memory[j+14];
  						sum15 += memory[j+15];
  	
  						sum += (sum8 + sum9 + sum10 + sum11 + sum12 + sum13 + sum14 + sum15);
  					}
				devnull = sum;
				//foo(sum);

				break;
			case Store:
				if(vect_loop) {
					//for (unsigned int i = 0; i < bound; i++)
					//for (unsigned int i = 0, j = block_start; i < K; i++,j=block_start + (j+1)%(block_size)) //FIXME: adjust boundaries and j range
					//	memory[j] = 1;//FIXME: change 1 to non-constant. it is equal to memcpy.

					printf("vectorized stream\n");

					for (unsigned int k = 0; k < K/block_size; k++) {
//#pragma ivdep 
//#pragma simd
						for (unsigned int i = block_start; i < block_start + block_size; i++) //FIXME: adjust boundaries and j range
						//for (unsigned int i = 0, j = 0; i < 100; i++, j++) //FIXME: adjust boundaries and j range
						{
							memory[i] = 1;
						}
					}
				}
				else

					for (unsigned int i = 0, j = block_start; i < K/16; i++,j=block_start + (j+16)%(block_size)) //FIXME: adjust boundaries and j range
					{
						memory[j] = 1;
						memory[j+1] = 1;
						memory[j+2] = 1;
						memory[j+3] = 1;
						memory[j+4] = 1;
						memory[j+5] = 1;
						memory[j+6] = 1;
						memory[j+7] = 1;
	
						memory[j+8] = 1;
						memory[j+9] = 1;
						memory[j+10] = 1;
						memory[j+11] = 1;
						memory[j+12] = 1;
						memory[j+13] = 1;
						memory[j+14] = 1;
						memory[j+15] = 1;
					}
				break;
			default:
				break;
		}
	}
};

//-----------------------------------------------------------------------------
// RandomThread implements thread for random access
//-----------------------------------------------------------------------------
class RandomThread : public DISThread
{
private:

//-----------------------------------------------------------------------------
// Random generator (orgininally taken from HPCC)
//-----------------------------------------------------------------------------
typedef unsigned long u64;
typedef long s64;

#define POLY 0x0000000000000007UL
#define PERIOD 1317624576693539401L
#define ZERO64B 0L

#define LCG_MUL64 6364136223846793005UL
#define LCG_ADD64 1L

u64 HPCC_starts(s64 n)
{
    int i, j;
    u64 m2[64];
    u64 temp, ran;
    while (n < 0) n += PERIOD;
    while (n > PERIOD) n -= PERIOD;
    if (n == 0) return 0x1;

    temp = 0x1;
    for (i=0; i<64; i++) {
        m2[i] = temp;
        temp = (temp << 1) ^ ((s64) temp < 0 ? POLY : 0);
        temp = (temp << 1) ^ ((s64) temp < 0 ? POLY : 0);
    }
    for (i=62; i>=0; i--)
        if ((n >> i) & 1)
            break;

    ran = 0x2;
    while (i > 0)
    {
        temp = 0;
        for (j=0; j<64; j++)
            if ((ran >> j) & 1)
                temp ^= m2[j];
        ran = temp;
        i -= 1;
        if ((n >> i) & 1)
            ran = (ran << 1) ^ ((s64) ran < 0 ? POLY : 0);
    }

    return ran;
}
#define NEXT_RAN ran = ((ran << 1)^((s64)ran < ZERO64B ? POLY : ZERO64B))
void get_mask64(unsigned long long N, u64 *mask)
{
    unsigned k=0;
    while(((u64)2<<k) <= N) k++;
    *mask = (2<<(k-1)) - 1;
}

public:
    RandomThread(int threadid, word_t *memory, int cpu_affinity) : 
		DISThread(threadid, memory, cpu_affinity) {}
	~RandomThread() {}

    void runtest()
    {
        if(use_index) {

            for(unsigned int i = 0; i < K; i++)
                memory[index[i]]++;
		}
        else
        {
            u64 ran = HPCC_starts(time(NULL)+pthread_self());
            u64 mask;
            unsigned r1;
            get_mask64(N, &mask);
			word_t sum = 0;

			switch (optype) {
				case Load:
					for(unsigned long long i = 0; i < K; i++)
					{
						NEXT_RAN;
						r1 = ran & mask;
						sum += memory[r1];
					}
					break;
				case Store:
					for(unsigned long long i = 0; i < K; i++)
					{
						NEXT_RAN;
						r1 = ran & mask;
						memory[r1]=1;
					}
					break;
				case LoadModifyStore:
					for(unsigned long long i = 0; i < K; i++)
					{
						NEXT_RAN;
						r1 = ran & mask;
						memory[r1]++;
					}
					break;
				default:
					fprintf(stderr, "Unexpected optype value\n");
				break;
			}

			devnull = sum;
        }
    }
};

//-----------------------------------------------------------------------------
// StrideThread implements thread for random access
//-----------------------------------------------------------------------------
class StrideThread : public DISThread
{
private:

public:
    StrideThread(int threadid, word_t *memory, int cpu_affinity) : 
		DISThread(threadid, memory, cpu_affinity) {}
	~StrideThread() {}

    void runtest()
    {
		switch (optype) {
			case Load:
				switch(unroll_depth) {
					case 1:
						runtest__load();
						break;
					case 4:
						runtest__load_unrolled4();
						break;
					case 8:
						runtest__load_unrolled8();
						break;
					case 16:
						runtest__load_unrolled16();
						break;
					default:
						fprintf(stderr, "Unexpected unroll_depth value\n");
						exit(1);
						break;
				}
				break;
			case Store:
				switch(unroll_depth) {
					case 1:
						runtest__store();
						break;
					case 4:
						runtest__store_unrolled4();
						break;
					case 8:
						runtest__store_unrolled8();
						break;
					case 16:
						runtest__store_unrolled16();
						break;
					default:
						fprintf(stderr, "Unexpected unroll_depth value\n");
						exit(1);
						break;
				}
				break;
			default:
				fprintf(stderr, "Unexpected optype value\n");
				break;
		}
    }

	void runtest__load() 
	{
		word_t sum = 0;
		for (unsigned int i = 0, j = start_offset*threadid ; i < K; i++,j=(j+S)%N) {
			sum += memory[j];
		}
		devnull = sum;
	}
	void runtest__load_unrolled4() 
	{
		word_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
		for (unsigned int i = 0, j = start_offset*threadid; i < K/4; i++,j=(j+4*S)%N) {
			sum0 += memory[j];
			sum1 += memory[(j+1*S)%N];
			sum2 += memory[(j+2*S)%N];
			sum3 += memory[(j+3*S)%N];
		}
		devnull = sum0 + sum1 + sum2 + sum3;
	}
	void runtest__load_unrolled8() 
	{
		word_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
		word_t sum4 = 0, sum5 = 0, sum6 = 0, sum7 = 0;
		for (unsigned int i = 0, j = start_offset*threadid; i < K/8; i++,j=(j+8*S)%N) {
			sum0 = memory[j];
			sum1 = memory[(j+1*S)%N];
			sum2 = memory[(j+2*S)%N];
			sum3 = memory[(j+3*S)%N];
			sum4 = memory[(j+4*S)%N];
			sum5 = memory[(j+5*S)%N];
			sum6 = memory[(j+6*S)%N];
			sum7 = memory[(j+7*S)%N];
		}
		devnull = sum0 + sum1 + sum2 + sum3;
		devnull += sum4 + sum5 + sum6 + sum7;
	}
	void runtest__load_unrolled16() 
	{
		word_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
		word_t sum4 = 0, sum5 = 0, sum6 = 0, sum7 = 0;
		word_t sum8 = 0, sum9 = 0, sum10 = 0, sum11 = 0;
		word_t sum12 = 0, sum13 = 0, sum14 = 0, sum15 = 0;
		for (unsigned int i = 0, j = start_offset*threadid; i < K/16; i++,j=(j+16*S)%N) {
			sum0 = memory[j];
			sum1 = memory[(j+1*S)%N];
			sum2 = memory[(j+2*S)%N];
			sum3 = memory[(j+3*S)%N];
			sum4 = memory[(j+4*S)%N];
			sum5 = memory[(j+5*S)%N];
			sum6 = memory[(j+6*S)%N];
			sum7 = memory[(j+7*S)%N];
			sum8 = memory[(j+8*S)%N];
			sum9 = memory[(j+9*S)%N];
			sum10 = memory[(j+10*S)%N];
			sum11 = memory[(j+11*S)%N];
			sum12 = memory[(j+12*S)%N];
			sum13 = memory[(j+13*S)%N];
			sum14 = memory[(j+14*S)%N];
			sum15 = memory[(j+15*S)%N];
		}
		devnull = sum0 + sum1 + sum2 + sum3;
		devnull += sum4 + sum5 + sum6 + sum7;
		devnull += sum8 + sum9 + sum10 + sum11;
		devnull += sum12 + sum13 + sum14 + sum15;
	}

	void runtest__store() 
	{
		for (unsigned int i = 0, j = start_offset*threadid; i < K; i++,j=(j+S)%N) {
			memory[j] = 1;
		}
	}
	void runtest__store_unrolled4() 
	{
		for (unsigned int i = 0, j = start_offset*threadid; i < K/4; i++,j=(j+4*S)%N) {
			memory[j] = 1;
			memory[(j+1*S)%N] = 1;
			memory[(j+2*S)%N] = 1;
			memory[(j+3*S)%N] = 1;
		}
	}
	void runtest__store_unrolled8() 
	{
		for (unsigned int i = 0, j = start_offset*threadid; i < K/8; i++,j=(j+8*S)%N) {
			memory[j] = 1;
			memory[(j+1*S)%N] = 1;
			memory[(j+2*S)%N] = 1;
			memory[(j+3*S)%N] = 1;
			memory[(j+4*S)%N] = 1;
			memory[(j+5*S)%N] = 1;
			memory[(j+6*S)%N] = 1;
			memory[(j+7*S)%N] = 1;
		}
	}
	void runtest__store_unrolled16() 
	{
		for (unsigned int i = 0, j = start_offset*threadid; i < K/16; i++,j=(j+16*S)%N) {
			memory[j] = 1;
			memory[(j+1*S)%N] = 1;
			memory[(j+2*S)%N] = 1;
			memory[(j+3*S)%N] = 1;
			memory[(j+4*S)%N] = 1;
			memory[(j+5*S)%N] = 1;
			memory[(j+6*S)%N] = 1;
			memory[(j+7*S)%N] = 1;
			memory[(j+8*S)%N] = 1;
			memory[(j+9*S)%N] = 1;
			memory[(j+10*S)%N] = 1;
			memory[(j+11*S)%N] = 1;
			memory[(j+12*S)%N] = 1;
			memory[(j+13*S)%N] = 1;
			memory[(j+14*S)%N] = 1;
			memory[(j+15*S)%N] = 1;
		}
	}
};


unsigned long long apply_scale(unsigned long long val, char scale)//FIXME: is it necessary for val to be ull? it used with smaller sizes everywhere
{
	switch(scale) 
	{
		case 'k': case 'K':
			return val * (1<<10);
		case 'm': case 'M':
			return val * (1<<20);
		case 'g': case 'G':
			return val * (1<<30);
		default:
			return val;
	}
}

int getint(char *str) 
{
	int val = 0;
	int len = 0;
	int factor;

	for (len = 0; (str[len] >= '0') && (str[len] <= '9'); len++) {
		printf("%c - %d\n",str[len],str[len]);
	}

	for (int i = 0; i < len; i++) {
		factor = 1;
		for (int j = 0; j < i; j++)
			factor = factor * 10;
		val += str[len-1-i] * factor; 
	}

	return val;
}

int parse_int_string(char *str, int *values, int *count, int max)
{
	int i = 0, j = 0;
	char val[3];
	*count = 0;
	//bool dash = false;

	while (str[i] != '\0')
		switch(str[i]) {
			case ',':
				if (*count < max) {
					values[(*count)++] = atoi(val);
					j=0;i++;
				}
				else
					return -1;
				break;
			case '-':
				if (*count < max) {
					values[(*count)++] = atoi(val);
					j=0;i++;

					/*if (dash) {
						for (int i = val_start; i <= atoi(val); i++)
							if (*count < max) {
								values[(*count)++] = i;
							}
							else
								return -1;
						dash = false;
					}
					val_start = atoi(val);
					dash=true;*/
				}
				else
					return -1;
				break;
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				val[j++]=str[i++];
				break;
			default:
				return -1;
		}

	if (j > 0) {
		values[(*count)++] = atoi(val);
	}
	
	return 0;
}

void allocate_memory() 
{
	if (numa_policy == Blocked) {
		// NUMA policy is blocked
	}
	else 
	if (numa_policy == Interleaved) 
	{
		// NUMA policy is interleaved
	} 
	else {
		memory = new word_t [N];
		/*for (unsigned long long i = 0; i < N; i+=(PAGE_SIZE/sizeof(word_t)))
		  memory[i] = 1;*/

		cpu_set_t cpuset0, cpuset1;
		CPU_ZERO(&cpuset0);
		CPU_ZERO(&cpuset1);
		CPU_SET(0,&cpuset0);
		CPU_SET(6,&cpuset1);

		sched_setaffinity(getpid(), sizeof(cpu_set_t), &cpuset0);
		for (unsigned long long i = 0; i < N/2; i+=(PAGE_SIZE/sizeof(word_t)))
			memory[i] = 1;

		sched_setaffinity(getpid(), sizeof(cpu_set_t), &cpuset1);
		for (unsigned long long i = N/2; i < N; i+=(PAGE_SIZE/sizeof(word_t)))
			memory[i] = 1;
	}
}

void free_memory()
{
	delete[] memory;
}

int main(int argc, char **argv)
{
	//int retval;
    char c;
    Timer timer;

	// set default values
	for (int i = 0; i < thread_count; i++)
		thread_affinity[i]=-1;

    // Parse parameters: disbench <testname> [OPTIONS]
	if (argc < 2) {
		fprintf(stderr, "Testname is not specified, use --help\n");
		exit(1);
	}

	if (!strcmp(argv[1],"stream")) {
		testtype = Stream;
	} else
	if (!strcmp(argv[1],"random")) {
		testtype = Random;
	} else
	if (!strcmp(argv[1],"stride")) {
		testtype = Stride;
	} else {
		testtype = Unknown;
	}

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{"mem", 		1, 0, 'm'},
			{"cores", 		1, 0, 'c'},
			{"papi", 		1, 0, 'p'},
			{"optype",		1, 0, 'o'}, 
			{"index",		0, 0, 'i'},
			{"stride",		1, 0, 's'},
			{"start-offset",1, 0, 'S'},
			{"unroll-depth",1, 0, 'u'},
			{"shared",		0, 0, 'q'}, 
			{"numa-nodes",	1, 0, 'w'}, 
			{"numa-policy",	1, 0, 'e'}, 
			{"numa-block",	1, 0, 'r'}, 
			{"version", 	0, 0, 'v'},
			{"help", 		0, 0, 'h'},
			{"vectorize",   0, 0, 'z'},
			{"test",        1, 0, 't'}
		};

		c = getopt_long(argc, argv, "s:m:c:k:o:u:vhzt:",
				long_options, &option_index);
		if (c == -1)
			break;

		char scale;
		char Nmin_scale, Nmax_scale, Nstep_scale;
		char Nmin_str[20], Nmax_str[20], Nstep_str[20];
		char Smin_scale, Smax_scale, Sstep_scale;
		char Smin_str[20], Smax_str[20], Sstep_str[20];

		switch (c) {
			case 'm':
				sscanf(optarg, "%[^.]..%[^:]:%s",Nmin_str,Nmax_str,Nstep_str);

				Nmin_scale = Nmax_scale = Nstep_scale = '0';

				sscanf(Nmin_str,"%llu%c",&Nmin,&Nmin_scale);
				Nmin = apply_scale(Nmin,Nmin_scale);

				sscanf(Nmax_str,"%llu%c",&Nmax,&Nmax_scale);
				Nmax = apply_scale(Nmax,Nmax_scale);

				sscanf(Nstep_str,"%llu%c",&Nstep,&Nstep_scale);
				Nstep = apply_scale(Nstep,Nstep_scale);

				break;
			case 's':
				sscanf(optarg, "%[^.]..%[^:]:%s",Smin_str,Smax_str,Sstep_str);

				Smin_scale = Smax_scale = Sstep_scale = '0';

				sscanf(Smin_str,"%llu%c",&Smin,&Smin_scale);
				Smin = apply_scale(Smin,Smin_scale);

				sscanf(Smax_str,"%llu%c",&Smax,&Smax_scale);
				Smax = apply_scale(Smax,Smax_scale);

				sscanf(Sstep_str,"%llu%c",&Sstep,&Sstep_scale);
				Sstep = apply_scale(Sstep,Sstep_scale);

				break;
			case 'o':
				if (!strcmp(optarg,"load")) {
					optype = Load;
				} else
				if (!strcmp(optarg,"store")) {
					optype = Store;
				} else
				if (!strcmp(optarg,"load-modify-store")) {
					optype = LoadModifyStore;
				} else {
					fprintf(stderr, "Unsupported operation type %s, use --help\n", optarg);
					exit(1);
				}

				break;
			case 'c':
				// --cores, -c is set
				if (parse_int_string(optarg,thread_affinity,&thread_count,MAX_CPU_CORES))
					fprintf(stderr, "--cores argument is incorrect\n");
				break;
			case 'p':
#ifdef PAPI_USED
				if (strstr(optarg,"tlb")) {
					papi_events[papi_counter] = PAPI_TLB_DM;
					papi_counter++;
				}
				if (strstr(optarg,"l1")) {
					papi_events[papi_counter] = PAPI_L1_TCM;
					papi_counter++;
				}
				if (strstr(optarg,"l2")) {
					papi_events[papi_counter] = PAPI_L2_TCM;
					papi_counter++;
				}
				if (strstr(optarg,"l3")) {
					papi_events[papi_counter] = PAPI_L3_TCM;
					papi_counter++;
				}
#endif
				break;
			case 'u':
				unroll_depth = atoi(optarg);
				if ((unroll_depth != 4) && (unroll_depth != 8) && (unroll_depth != 16)) {
					fprintf(stderr, "Unroll depth %d is not supported. Possible values are 4, 8 or 16\n", unroll_depth);
					exit(1);
				}
				break;
			case 'k':
				// -K is set
				scale = '0';
				sscanf(optarg,"%llu%c",&K,&scale);
				K = apply_scale(K,scale);
				break;
			case 'S':
				// --start-offset is set
				scale = '0';
				sscanf(optarg,"%llu%c",&start_offset,&scale);
				start_offset = apply_scale(start_offset,scale);
				break;
			case 'i':
				// --index is set
				use_index = true;
				break;
			case 'q':
				// --shared is set
				shared_memory = true;
				break;
			case 'w':
				// --numa-nodes is set
				if (parse_int_string(optarg,numa_nodes,&numa_nodes_count,MAX_NUMA_NODES))
					fprintf(stderr, "--numa-nodes argument is incorrect\n");
				break;
			case 'e':
				// --numa-policy is set
				if (!strcmp(optarg,"blocked")) {
					numa_policy = Blocked;
				} else
				if (!strcmp(optarg,"interleaved")) {
					numa_policy = Interleaved;
				} else {
					fprintf(stderr, "Unexpected NUMA policy %s, use --help\n", optarg);
					exit(1);
				}
				break;
			case 'r':
				// --numa-block is set
				char scale;
				sscanf(optarg,"%llu%c",&numa_block_size,&scale);//FIXME: numa_block_size must be 64 bits long. its int now
				numa_block_size = apply_scale(numa_block_size,scale);
				break;
			case 'v':
				printf("disbench version %d.%d\n",VERSION,REVISION);
				exit(0);
			case 'h':
				printf("Usage:\n");
				printf("   disbench <test> [OPTIONS]\n\n");
				printf("Options:\n");
				printf("   test                        \tpossible tests are 'stream', 'stride' or 'random'.                  \n");
				printf("   --optype <OPTYPE>           \tspecifies type of operation for memory accesses: load (default),    \n");
				printf("                               \tstore, load-modify-store.                                           \n");
				printf("   -m,--mem <Nmin..Nmax:Nstep> \tmemory region which desired to be used for testing. For example,    \n");
				printf("                               \t1..1024:1 means that from 1 to 1024 words will be used as memory    \n");
				printf("                               \tfor testing with step of 1 word. Specifiers are also possible:      \n");
				printf("                               \tkilo ('k' or 'K'), mega ('m' or 'M') and giga ('g' or 'G').         \n");
				printf("   -p,--papi <PERFCOUNTERS>    \tenables performance counter monitoring, available only following    \n");
				printf("                               \tcounters: l1 cache misses (l1), l2 cache misses (l2), l3 cache      \n");
				printf("                               \tmisses (l3), tlb misses (tlb). Note that tlb can not be used with   \n");
				printf("                               \tl1, l2, l3 at the same time.i                                       \n");
				printf("   -c,--cores <CORES>          \tspecifies cores which are to be used for testing                    \n");
				printf("   --numa-nodes <NODES>        \tspecifies NUMA nodes used for memory allocation                     \n");
				printf("   --numa-policy <TYPE>        \tspecifies NUMA distribution type, possible types are 'blocked',     \n");
				printf("                               \t'interleaved'                                                       \n");
				printf("   --numa-block <block>        \tspecifies size of memory block used for interleaved distribution    \n");
				printf("   -k <K>                      \tspecifies number of iterations. Specifiers are also possible: kilo  \n");
				printf("                               \t('k' or 'K'), mega ('m' or 'M') and giga ('g' or 'G').              \n");
				printf("   --unroll-depth <DEPTH>      \tspecifies depth of manual loop unrolling (feasible only for stream  \n");
				printf("                               \tand stride tests). Possible values are 4, 8, 16.                    \n");
				printf("   --shared                    \tenables sharing of memory between cores.                            \n");
				printf("   -v,--version                \tprints version                                                      \n");
				printf("   -h,--help                   \tprints help information                                             \n");
				printf("Options specific for random test:\n");
				printf("   -i,--index                  \tenable index vector usage, if this option is not used then HPCC     \n");
				printf("                               \trandom generator is used instead.                                   \n");
				printf("Options specific for stride test:\n");
				printf("   -s,--stride <Smin..Smax:Sstep> stride variation interval which is desired to be used for testing. \n");
				printf("                               \tSensible only for stride test. Specifiers are also possible: kilo   \n");
				printf("                               \t('k' or 'K'), mega ('m' or 'M') and giga ('g' or 'G').              \n");
				printf("   --start-offset <Offset>     \tstart offset of threads, first thread will start from memory[0],    \n");
				printf("                               \tsecond from memory[0+Offset] etc. Only reasonable if shared memory  \n");
				printf("                               \tis used.                                                            \n");                      
				printf("Report bugs, ideas and propositions to alexndr.frolov@gmail.com                                      \n");                      
				exit(0);
			case 'z':
				vect_loop = true;
				printf("loop vectorization will be used\n");
				if(testtype != Stream)
					printf("Warning: --vectorize(-z) key make sense for test Stream only\n");
				break;
			case 't':
				// FIXME: For MIC only
				for (int i = 0; i < atoi(optarg); i++)
					thread_affinity[i] = i;
				thread_count = atoi(optarg);
				break;
			default:
				fprintf(stderr, "Unknown option, use --help\n");
				exit(1);
		}
	}

	if (testtype == Unknown) {
		fprintf(stderr, "Unkown test name, use --help\n");
		exit(1);
	}

	// Print settings
	printf("disbench v%d.%d:\n",VERSION,REVISION);
	printf("  Test: \t\t\t%s\n", TESTNAME);
	printf("  Opertation: \t\t\t%s\n", OPTYPENAME);
	printf("  Memory range (min..max:step): %llu..%llu:%llu\n", Nmin, Nmax, Nstep);
	printf("  Stride range (min..max:step): %llu..%llu:%llu\n", Smin, Smax, Sstep);
	printf("  Start offset:\t\t\t%llu\n", start_offset);
	printf("  Interations (K): \t\t%llu\n", K);
	printf("  Loop unroll depth: \t\t%d\n", unroll_depth);
	printf("  Number of threads: \t\t%d\n", thread_count);
	printf("  Threads affinity:  \t\t");
	for (int i = 0; i < thread_count; i++) {
		if (i > 0) printf(", ");
		if (thread_affinity[i] != -1) 
		   printf("%d(CPU%d)",i,thread_affinity[i]);
		else
		   printf("%d(auto)",i);
	}
	printf("\n");
	printf("  NUMA nodes:  \t\t\t");
	for (int i = 0; i < numa_nodes_count; i++) {
		if (i > 0) printf(", ");
		printf("%d",numa_nodes[i]);
	}
	printf("\n");
	if (numa_policy == Blocked)
		printf("  NUMA policy:  \t\t%s\n",numa_policies[numa_policy]);
	else
		printf("  NUMA policy:  \t\t%s, block size %llu\n",numa_policies[numa_policy],numa_block_size);
	
#ifdef PAPI_USED
	printf("  PAPI monitoring:   \t\t");
	for (int i = 0; i < papi_counter; i++) {
		if (i > 0) printf(", ");
		char event_name[25];
		papi_get_event_name(papi_events[i],event_name);
		printf("%s",event_name);
	}
	printf("\n");
#endif
	//exit(1);


	// pin main thread to the same core as thread 0
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(thread_affinity[0],&cpuset);
	pthread_setaffinity_np(pthread_self(),sizeof(cpu_set_t), &cpuset);

#ifdef PAPI_USED
	// Initilize PAPI library (low-level)
	int retval = PAPI_library_init(PAPI_VER_CURRENT);
	if (retval != PAPI_VER_CURRENT) {
		fprintf(stderr, "PAPI library init error!\n");
		exit(1);
	}

	retval = PAPI_thread_init(pthread_self);
	if (retval != PAPI_OK) {
		fprintf(stderr, "PAPI thread init error!\n");
		exit(1);
	}

#endif

    Thread **threads = new Thread * [thread_count];

	for (N = Nmin; N <= Nmax; N = (Nstep==0) ? N*2 : N+Nstep) {

			if (shared_memory) 
				allocate_memory();

			for (S = Smin; S <= Smax; S = (Sstep==0) ? S*2 : S+Sstep) {

				switch (testtype) {
					case Stream:
						for (int i = 0; i < thread_count; i++)
							threads[i] = new StreamThread(i, memory, thread_affinity[i]);
						break;
					case Random:
						for (int i = 0; i < thread_count; i++)
							threads[i] = new RandomThread(i, memory, thread_affinity[i]);
						break;
					case Stride:
						for (int i = 0; i < thread_count; i++)
							threads[i] = new StrideThread(i, memory, thread_affinity[i]);
						break;
					default:
						fprintf(stderr, "Unexpected testtype value\n");
						exit(1);
				}

				printf("--------------------------------------------------------------------------------------\n");
				printf("Running test with parameters:\n");
				printf("   allocated memory %lld/%lld/%lld bytes/Mbytes/Gbytes,",
						(long long)sizeof(word_t)*N,
						(long long)sizeof(word_t)*N/(1024*1024),
						(long long)sizeof(word_t)*N/(1024*1024*1024));
				printf(" %llu/%llu/%llu pages of 4K/1M/1G\n",
						sizeof(word_t)*N/(PAGE_SIZE),
						sizeof(word_t)*N/(HUGE_PAGE_SIZE),
						sizeof(word_t)*N/(GIGANTIC_PAGE_SIZE));
				printf("   stride size %lld/%lld/%lld bytes/Kbytes/Mbytes\n",
						(long long)sizeof(word_t)*S,
						(long long)sizeof(word_t)*S/(1024),
						(long long)sizeof(word_t)*S/(1024*1024));

				timer.begin();
				for (int i = 0; i < thread_count; i++)
					threads[i]->start();
				for (int i = 0; i < thread_count; i++)
					threads[i]->wait();
				timer.end();

				for (int i = 0; i < thread_count; i++)
					threads[i]->print_report();

			if (shared_memory) 
				free_memory();

			printf("Total: Bandwidth %.2f MW/s (%.2f MB/s), elapsed time %f sec\n", 
						(double)(K*thread_count)/(timer.gettime()*1024*1024),
						(double)(K*thread_count*8)/(timer.gettime()*1024*1024),
						timer.gettime());
				for (int i = 0; i < thread_count; i++)
					delete threads[i];

				if ((S == 0) && (Sstep == 0))
					break;
			}

			//if (shared_memory) delete[] memory;
	}
}
