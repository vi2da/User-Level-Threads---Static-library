/*
 * User-Level Threads Staitc Library (uthreads)
 * Author: Created by david.oh on 4/21/17.
 */
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <queue>
#include <sys/time.h>
#include <stdlib.h>
#include "uthreads.h"


/**********************************************************************************************/
/******************************        translet address         *******************************/
/**********************************************************************************************/


typedef unsigned long address_t;

#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
            "rol    $0x11,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
            "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}

#endif

/**********************************************************************************************/
/******************************             #DEFINE              ******************************/
/**********************************************************************************************/

#define MIN_LEGAL_ID 0
#define NOT_DEPEND -1
#define MAIN_TID 0
#define FIRST_PLC 0

#define STACK_SIZE 4096

//used for function that search the index of thread in vector.
#define NOT_FOUND -1

//use to return from function.
#define SUCCES 0
#define FAIL -1

//ese for the function that print to the user the error.
#define SYS_ERR 0
#define LIB_ERR 1

//use for the system call exit().
#define SUCCES_EXIT 0
#define FAILD_EXIT 1


#define MILLION 1000000
#define SIG_SUCCES 0

#define NON_POSITIVE 0
#define INIT_QUANTUM 0

/**********************************************************************************************/

enum state
{
    RUNNING,
    READY,
    BLOCKED,
    SYNCED,
    SYNC_BLOCK,
    TERMINAT
};

typedef enum state state;


/**********************************************************************************************/
/******************************          class Thread            ******************************/
/**********************************************************************************************/

class Thread{
private:
    char _stack[STACK_SIZE];
    void(*_f)(void);
    int _tid;

    // the tid of the thread That I depended on him, -1 if not depande.
    int _depand_tid;

    int _quatum = INIT_QUANTUM;
    state _state;

    // all the tid of the thread's That depend on me.
    std::vector<Thread *> _depand_on_me;

public:
    sigjmp_buf _env;
    Thread(int tid, void(*f)(void));

    state get_state() { return _state; };
    void set_state(state new_state) { _state = new_state; };

    int get_tid() { return _tid; };

    //Updating my dependency.
    void set_depand_tid(int new_dependence){ _depand_tid = new_dependence; };
    int get_depand_tid() { return _depand_tid; };

    //add a tid to be depand on me.
    void add_depand_on_me(Thread * to_add);
    //Deletes tid from list of depand on me.
    void delete_dependend_by_tid(int tid);

    //pass on all thread that depand on me, And updates each to the next state (to be not depant on me).
    void handle_depend_t(std::vector<Thread *> * add_here_ready_t);

    int get_quantum() { return _quatum; };
    // print all the tid of the thread that depand on me.
    void print_depande();

    void add_quanta() { _quatum++; };
};

/**********************************************************************************************/
/******************************         STATIC VARIABLE          ******************************/
/**********************************************************************************************/

//queue of the ready to run thread.
static std::vector<Thread *> ready;
//queue of all the thread that be initialize and not terminate. (all the exist thread).
static std::vector<Thread *> t_Prosses;
//pointer to the running thread.
static Thread * running = nullptr;
//min heap of all the tid which were in use that ther thread was terminate.
static std::priority_queue<int, std::vector<int>, std::greater<int> > free_used_tid;
static int total_quantums = 0;

//pointer to the thread that need to termiante. (used in case that thrade terminat itself,
//after the context switch need to delet him).
static Thread * terminate_t = nullptr;

/**********************************************************************************************/
/******************************         STATIC FUNCTION          ******************************/
/**********************************************************************************************/


/**
 * get the index of the thrade in the vecor fa.
 * @param fa - pointer to vector of pointer to thrad.
 * @param tid - the thead to find in fa.
 * @return if not exit in the vector -1, else the index.
 */
static int get_index(std::vector<Thread *> * fa, int tid)
{
    bool see_tid = false;
    int index = NOT_FOUND;
    for(std::vector<Thread *>::iterator it = fa->begin(); (it != fa->end()) && (!see_tid) ; ++it)
    {
        index++;
        if((*it)->get_tid() == tid) { see_tid = true; }
    }
    return see_tid ? index : NOT_FOUND;
}

/*************************************************/


/**
 * print the msg to stderr.
 * @param errType - can be system / library error
 * @param msg - the message to print.
 */
static void processErrors(const int errType, std::string msg)
{
    switch(errType){
        case SYS_ERR:
            msg = "system error: " + msg + "\n";
            break;

        case LIB_ERR:
            msg = "thread library error: " + msg + "\n";
            break;

        default:
            return;
    }
    std::cerr << msg;
    fflush(stderr);
    return;
}

/*************************************************/


/**
 * get the next min free tid, if not free tid return -1.
 * @return - the next min free tid, if not free tid return -1.
 */
static int get_newlegal_tid()
{
    //the first legal tid (Except from the main thrade).
    static int next_legal_tid = 1;

    int to_return;
    if(free_used_tid.empty())
    {
        if(next_legal_tid >= MAX_THREAD_NUM) { return FAIL; }

        int fresh_tid = next_legal_tid;
        next_legal_tid++;
        return fresh_tid;
    }
    to_return = free_used_tid.top();
    free_used_tid.pop();
    return to_return;
}

/*************************************************/


/**
 * in case of the library need to termine itself this function delet all the memory allocation, and call the system call
 * exit() with the given int exit_state.
 * @param exit_state - the number to exit with.
 */
static void del_library(int exit_state)
{

    for(int i = 0; i < (int)t_Prosses.size(); i++){
        delete t_Prosses[i];
        t_Prosses[i] = nullptr;
    }
    ready.clear();
    t_Prosses.clear();
    exit(exit_state);
}


/*************************************************/


static sigset_t g_set;

/**
 * mask the SIGVTALRM signale.
 */
static void mask_sig()
{
    sigemptyset(&g_set);
    sigaddset(&g_set, SIGVTALRM);
    if(sigprocmask(SIG_BLOCK, &g_set, NULL) != SUCCES)
    {
        processErrors(SYS_ERR, "sys can't block the signal's.");
        del_library(FAILD_EXIT);
    }
}


/**
 * unmask the SIGVTALRM signale.(after call this function all the signal that whas block will show,
 * and not delete from pending signal).
 */
static void unmask_sig()
{
    sigemptyset(&g_set);
    sigaddset(&g_set, SIGVTALRM);
    if(sigprocmask(SIG_UNBLOCK, &g_set, NULL) != SUCCES)
    {
        processErrors(SYS_ERR, "sys can't unblock the signal's.");
        del_library(FAILD_EXIT);
    }
}

/**
 * delete from vector, and check if not occure error.
 * @param vec to delete from.
 * @param index the index to delete.
 */
void safe_erase(std::vector<Thread*> * vec, int index)
{
    try
    {
        vec->erase(vec->begin() + index);
    }
    catch(int e)
    {
        processErrors(SYS_ERR, "problem");
        del_library(FAILD_EXIT);
    }

}


/**
 * push to the end of the vector, and check if not occure error.
 * @param vec to push into.
 * @param to_push the element to push.
 */
void safe_push_back(std::vector<Thread*> * vec, Thread * to_push)
{
    try
    {
        vec->push_back(to_push);
    }
    catch(int e)
    {
        processErrors(SYS_ERR, "problem");
        del_library(FAILD_EXIT);
    }
}

/*************************************************/


static struct itimerval timer;

//is define only for this function.
#define FROM_LONG_JUMP 5

/**
 * the function to handling the SIGVTALRM
 * @param sig - int
 */
static void timer_handler(int sig)
{
    if(sig != SIGVTALRM)
    {
        return;
    }

    if(ready.empty())
    {
        running->add_quanta();
        total_quantums++;
        return;
    }

    if(running->get_state() == RUNNING)
    {
        running->set_state(READY);
        safe_push_back(&ready, running);
    }

    total_quantums++;
    Thread* prev_t = running;

    running = ready[FIRST_PLC];
    safe_erase(&ready, FIRST_PLC);
    running->set_state(RUNNING);
    running->add_quanta();

    int temp = sigsetjmp(prev_t->_env, 1);
    if(temp == FROM_LONG_JUMP)
    {
        if(terminate_t != nullptr)
        {
            delete terminate_t;
            terminate_t = nullptr;
        }

        if (setitimer (ITIMER_VIRTUAL, &timer, NULL))
        {
            processErrors(SYS_ERR, "error occure in seting timer");
            del_library(FAILD_EXIT);
            return;
        }

        sigset_t pend_set;
        sigemptyset(&pend_set);
        sigpending(&pend_set);

        if(sigismember(&pend_set, SIGVTALRM))
        {
            int signal;
            sigwait(&pend_set, &signal);
        }

        return;
    }

    siglongjmp(running->_env, FROM_LONG_JUMP);
}

/*************************************************/

/**
 * set the timer to raise a SIGVTALRM every quantum_usecs, and set the timer_handler to SIGVTALRM.
 * @param quantum_usecs - int to be the maximum time that thread can be in RUNNING state before context switch.
 * @return - 0 if succes, -1 on faild.
 */
static int set_alarm(int quantum_usecs)
{
    struct sigaction sa;

    // Install timer_handler as the signal handler for SIGVTALRM.
    sa.sa_handler = &timer_handler;
    sa.sa_flags = 0;
    sigfillset(&sa.sa_mask);

    if (sigaction(SIGVTALRM, &sa, NULL) != SIG_SUCCES)
    {
        processErrors(LIB_ERR, "error in creating the handler");
        return FAIL;
    }

    // Configure the timer to expire after quantum_usecs.
    timer.it_value.tv_sec = quantum_usecs / MILLION;
    timer.it_value.tv_usec = quantum_usecs % MILLION;

    // configure the timer to expire every quantum_usecs after that.
    timer.it_interval.tv_sec = quantum_usecs / MILLION;
    timer.it_interval.tv_usec = quantum_usecs % MILLION;

    // Start a virtual timer. It counts down whenever this process is executing.
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL) != SIG_SUCCES)
    {
        processErrors(LIB_ERR, "error in creating the timer");
        return FAIL;
    }

    return SUCCES;
}


/**********************************************************************************************/
/******************************          class Thread            ******************************/
/**********************************************************************************************/

/**
 * print all the tid of the thread that depand on me.
 */
void Thread::print_depande()
{
    for (std::vector<Thread *>::iterator it = this->_depand_on_me.begin(); it != this->_depand_on_me.end(); ++it)
    {
        std::cout << (*it)->get_tid() << "; ";
    }
    std::cout << std::endl;

}

/*******************************************************/


/**
 * pass on all thread that depand on me, And updates each to the next state (to be not depant on me).
 * @param add_here_ready_t - the vector to add the ready threads.
 */
void Thread::handle_depend_t(std::vector<Thread *> *add_here_ready_t)
{
    for(int i = 0; i < (int)this->_depand_on_me.size(); i++)
    {
        if(this->_depand_on_me[i]->get_state() == SYNCED)
        {
            safe_push_back(add_here_ready_t, this->_depand_on_me[i]);
            this->_depand_on_me[i]->set_state(READY);
        }

        if(this->_depand_on_me[i]->get_state() == SYNC_BLOCK)
        {
            this->_depand_on_me[i]->set_state(BLOCKED);
        }

        this->_depand_on_me[i]->set_depand_tid(NOT_DEPEND);
        this->_depand_on_me[i] = nullptr;
    }

    this->_depand_on_me.clear();
}

/*******************************************************/


/**
 *
 * @param tid
 */
void Thread::delete_dependend_by_tid(int tid)
{
    int index = get_index(&this->_depand_on_me, tid);
    if(index == NOT_FOUND) { return; }

    this->_depand_on_me[index] = nullptr;
    safe_erase(&this->_depand_on_me, index);
}

/*******************************************************/


/**
 * add a tid to be depand on me.
 * @param to_add the thread that depend on me, to add to my dependent list.
 */
void Thread::add_depand_on_me(Thread * to_add)
{
    int index = get_index(&this->_depand_on_me, to_add->get_tid());
    if(index == NOT_FOUND)
    {
        safe_push_back(&this->_depand_on_me, to_add);
    }
}

/*******************************************************/


/**
 * constractor.
 * @param tid the id to the new thread.
 * @param f pointer to the thread function.
 */
Thread::Thread(int tid, void (*f)(void))
{
    if(tid != MAIN_TID)
    {
        _state = READY;
        _f = f;
        address_t sp = (address_t)_stack + STACK_SIZE - sizeof(address_t);
        address_t pc = (address_t)_f;
        sigsetjmp(_env, 1);
        (_env->__jmpbuf)[JB_SP] = translate_address(sp);
        (_env->__jmpbuf)[JB_PC] = translate_address(pc);
        sigemptyset(&_env->__saved_mask);
    }

    _tid = tid;
    _depand_tid = NOT_DEPEND;

    if(tid == MAIN_TID)
    {
        _state = RUNNING;
    }
}


/**********************************************************************************************/
/******************************         UTHREAD LIBRARY          ******************************/
/**********************************************************************************************/


/**
 * Description: This function initializes the thread library.
 * You may assume that this function is called before any other thread library
 * function, and that it is called exactly once. The input to the function is
 * the length of a quantum in micro-seconds. It is an error to call this
 * function with non-positive quantum_usecs.
 *
 * @param quantum_usecs to the max time that the thread can use the cpu without interepsion.
 * @return  On success, return 0. On failure, return -1.
 */
int uthread_init(int quantum_usecs)
{

    if(quantum_usecs <= NON_POSITIVE)
    {
        processErrors(LIB_ERR, "quantum_usecs must be positive.");
        return FAIL;
    }

    Thread * main_t = new (std::nothrow) Thread(MAIN_TID, nullptr);
    if(!main_t)
    {
        processErrors(SYS_ERR, "the system faild in alloc memory.");
        return FAIL;
    }

    total_quantums++;
    safe_push_back(&t_Prosses, main_t);
    running = main_t;

    if(set_alarm(quantum_usecs) == FAIL)
    {
        running = nullptr;
        t_Prosses.clear();
        delete main_t;
        total_quantums--;
        return FAIL;
    }
    running->add_quanta();

    return SUCCES;
}

/**********************************************************************************************/


/**
 * Description: This function creates a new thread, whose entry point is the
 * function f with the signature void f(void). The thread is added to the end
 * of the READY threads list. The uthread_spawn function should fail if it
 * would cause the number of concurrent threads to exceed the limit
 * (MAX_THREAD_NUM). Each thread should be allocated with a stack of size
 * STACK_SIZE bytes.
 *
 * @param f pointer to the thread function.
 * @return On success, return the ID of the created thread.
 * On failure, return -1.
 */
int uthread_spawn(void (*f)(void))
{
    mask_sig();

    int tid = get_newlegal_tid();
    if(tid == FAIL)
    {
        processErrors(LIB_ERR, "exceed the maximum allow threade.");
        unmask_sig();
        return FAIL;
    }

    Thread * new_born = new (std::nothrow) Thread(tid, f);

    if(!new_born)
    {
        processErrors(SYS_ERR, "the system faild in alloc memory.");
        unmask_sig();
        return FAIL;
    }

    try
    {
        t_Prosses.push_back(new_born);
    }
    catch (int e)
    {
        free_used_tid.push(tid);
        delete new_born;

        processErrors(SYS_ERR, "the system faild in alloc memory.");
        unmask_sig();
        return FAIL;
    }

    try
    {
        ready.push_back(new_born);
    }
    catch(int e)
    {
        free_used_tid.push(tid);
        delete new_born;

        safe_erase(&t_Prosses, ((int)t_Prosses.size() - 1));
        processErrors(SYS_ERR, "the system faild in alloc memory.");
        unmask_sig();
        return FAIL;
    }

    unmask_sig();
    return tid;
}

/**********************************************************************************************/


/*

 * Return value:
*/
/**
 * Description: This function terminates the thread with ID tid and deletes
 * it from all relevant control structures. All the resources allocated by
 * the library for this thread should be released. If no thread with ID tid
 * exists it is considered as an error. Terminating the main thread
 * (tid == 0) will result in the termination of the entire process using
 * exit(0) [after releasing the assigned library memory].
 *
 * @param tid the id  of the treade to terminate.
 * @return The function returns 0 if the thread was successfully
 * terminated and -1 otherwise. If a thread terminates itself or the main
 * thread is terminated, the function does not return.
 */
int uthread_terminate(int tid)
{
    mask_sig();

    if (tid < MIN_LEGAL_ID || tid > MAX_THREAD_NUM)
    {
        processErrors(LIB_ERR, "not legale tid was sen't.");
        unmask_sig();
        return FAIL;
    }

    if (tid == MAIN_TID)
    {
        del_library(SUCCES_EXIT);
    }

    int index = get_index(&t_Prosses, tid);
    if (index == NOT_FOUND)
    {
        processErrors(LIB_ERR, "not exist tid was sen't to termine.");
        unmask_sig();
        return FAIL;
    }

    Thread *to_terminate = t_Prosses[index];

    safe_erase(&t_Prosses, index);

    if (to_terminate->get_state() == READY)
    {
        int indx = get_index(&ready, tid);
        safe_erase(&ready, indx);
    }

    if (to_terminate->get_depand_tid() != NOT_DEPEND)
    {
        int father_index = get_index(&t_Prosses, to_terminate->get_depand_tid());
        t_Prosses[father_index]->delete_dependend_by_tid(tid);
    }

    to_terminate->handle_depend_t(&ready);

    if(to_terminate->get_state() == RUNNING)
    {
        terminate_t = to_terminate;
        terminate_t->set_state(TERMINAT);
        raise(SIGVTALRM);
    }
    else
    {
        delete to_terminate;
    }
    free_used_tid.push(to_terminate->get_tid());

    unmask_sig();

    return SUCCES;
}

/**********************************************************************************************/


/**
 * Description: This function blocks the thread with ID tid. The thread may
 * be resumed later using uthread_resume. If no thread with ID tid exists it
 * is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision
 * should be made. Blocking a thread in BLOCKED state has no
 * effect and is not considered as an error.
 *
 * @param tid the id of the treade to block.
 * @return  On success, return 0. On failure, return -1.
 */
int uthread_block(int tid)
{
    mask_sig();

    if (tid < MIN_LEGAL_ID || tid > MAX_THREAD_NUM)
    {
        processErrors(LIB_ERR, "not legale tid was sen't.");
        unmask_sig();
        return FAIL;
    }

    if(tid == MAIN_TID)
    {
        processErrors(LIB_ERR, "it's illegal to block the main thread.");
        unmask_sig();
        return FAIL;
    }

    int index = get_index(&t_Prosses, tid);
    if(index == NOT_FOUND)
    {
        processErrors(LIB_ERR, "not exist tid was sen't to block.");
        unmask_sig();
        return FAIL;
    }

    Thread * t_to_block = t_Prosses[index];
    if(t_to_block->get_state() == BLOCKED)
    {
        unmask_sig();
        return SUCCES;
    }

    if(t_to_block->get_state() == SYNCED)
    {
        t_to_block->set_state(SYNC_BLOCK);
        unmask_sig();
        return SUCCES;
    }

    if(t_to_block->get_state() == RUNNING)
    {
        raise(SIGVTALRM);
    }

    if(t_to_block->get_state() == READY)
    {
        int indx = get_index(&ready, tid);
        safe_erase(&ready, indx);
    }

    t_to_block->set_state(BLOCKED);

    unmask_sig();
    return SUCCES;
}

/**********************************************************************************************/


/**
 * Description: This function resumes a blocked thread with ID tid and moves
 * it to the READY state. Resuming a thread in a RUNNING or READY state
 * has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered as an error.
 *
 * @param tid the id of the treade to resume.
 * @return On success, return 0. On failure, return -1.
 */
int uthread_resume(int tid)
{
    mask_sig();

    if (tid < MIN_LEGAL_ID || tid > MAX_THREAD_NUM)
    {
        processErrors(LIB_ERR, "not legale tid was sen't.");
        unmask_sig();
        return FAIL;
    }

    int index = get_index(&t_Prosses, tid);
    if(index == NOT_FOUND)
    {
        processErrors(LIB_ERR, "not exist tid was sen't to resume.");
        unmask_sig();
        return FAIL;
    }

    Thread * t_to_resume = t_Prosses[index];
    if(t_to_resume->get_state() == SYNC_BLOCK)
    {
        t_to_resume->set_state(SYNCED);
    }

    if(t_to_resume->get_state() == BLOCKED)
    {
        t_to_resume->set_state(READY);
        safe_push_back(&ready, t_to_resume);
    }

    unmask_sig();
    return SUCCES;
}

/**********************************************************************************************/


/**
 * Description: This function blocks the RUNNING thread until thread with
 * ID tid will move to RUNNING state (i.e.right after the next time that
 * thread tid will stop running, the calling thread will be resumed
 * automatically). If thread with ID tid will be terminated before RUNNING
 * again, the calling thread should move to READY state right after thread
 * tid is terminated (i.e. it won’t be blocked forever). It is considered
 * as an error if no thread with ID tid exists or if the main thread (tid==0)
 * calls this function or thread sync itself. Immediately after the RUNNING thread transitions to
 * the BLOCKED state a scheduling decision should be made.
 *
 * @param tid the id of the treade to resume.
 * @return On success, return 0. On failure, return -1.
 */
int uthread_sync(int tid)
{
    mask_sig();

    if(running->get_tid() == MAIN_TID)
    {
        processErrors(LIB_ERR, "can't sync the main thrade.");
        unmask_sig();
        return FAIL;
    }

    if(running->get_tid() == tid)
    {
        processErrors(LIB_ERR, "thrade can't sync itself.");
        unmask_sig();
        return FAIL;
    }

    int index = get_index(&t_Prosses, tid);
    if(index == NOT_FOUND)
    {
        processErrors(LIB_ERR, "not exist tid was sen't to sync.");
        unmask_sig();
        return FAIL;
    }

    Thread * father_t = t_Prosses[index];
    father_t->add_depand_on_me(running);

    running->set_depand_tid(father_t->get_tid());
    running->set_state(SYNCED);

    raise(SIGVTALRM);
    unmask_sig();

    return SUCCES;
}

/**********************************************************************************************/


/**
 * This function returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
 */
int uthread_get_tid() { return running->get_tid(); }

/**********************************************************************************************/


/**
 * This function returns the total number of quantums that were
 * started since the library was initialized, including the current quantum.
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number
 * should be increased by 1.
 *
 * @return The total number of quantums.
 */
int uthread_get_total_quantums(){ return total_quantums; }

/**********************************************************************************************/


/**
 * This function returns the number of quantums the thread with
 * ID tid was in RUNNING state. On the first time a thread runs, the function
 * should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state
 * when this function is called, include also the current quantum). If no
 * thread with ID tid exists it is considered as an error.
 *
 * @param tid the id of the treade to get its quantums.
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
 */
int uthread_get_quantums(int tid)
{
    mask_sig();

    if(tid < MIN_LEGAL_ID || tid > MAX_THREAD_NUM)
    {
        processErrors(LIB_ERR, "not legale tid was sen't.");
        unmask_sig();
        return FAIL;
    }

    int index = get_index(&t_Prosses, tid);
    if(index == NOT_FOUND)
    {
        processErrors(LIB_ERR, "not exist tid was sen't to get_quantum.");
        unmask_sig();
        return FAIL;
    }
    unmask_sig();
    return t_Prosses[index]->get_quantum();

}

/**********************************************************************************************/
/***************************         END UTHREAD LIBRARY          *****************************/
/**********************************************************************************************/