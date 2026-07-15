#include "lock.hpp"
#include <stdexcept>
#include <mutex>
#include <cstring>

class YangMutexHelper
{
public:
    bool getSetup(){
        return setup;
    }

    void init(size_t num_threads, size_t starting_thread_id)
    {
        // Zeroed: the ack check in lock() reads a rival's slot, and a rival
        // announced in competitors[] may not have reset its slot yet — with
        // malloc garbage that first-round read took the skip-ack path on
        // values that were never written.
        this->spinners = (volatile size_t *)malloc(sizeof(size_t) * num_threads);
        memset((void *)this->spinners, 0, sizeof(size_t) * num_threads);

        this->competitors = (volatile int *)malloc(sizeof(volatile int) * 2);
        (competitors)[0] = -1;
        (competitors)[1] = -1;
        tiebreaker= (volatile int *)malloc(sizeof(volatile int));
        *tiebreaker=-1;
        this->setup=true;

        this->num_threads = num_threads;
        this->starting_thread_id = starting_thread_id;
        
        right = new YangMutexHelper();
        left = new YangMutexHelper();
        if (num_threads - (int)(num_threads / 2) > 1)
        {
            right->init(num_threads - (int)(num_threads / 2), starting_thread_id + num_threads / 2);
        }

        if (num_threads / 2 > 1)
        {
            left->init(num_threads / 2, starting_thread_id);
        }
    }
    void lock(size_t thread_id)
    {

        // lock all subtrees
        if (side(thread_id) && right->getSetup())
        {
            // right->lock(thread_id);
            right->lock(thread_id);
        }
        if (!side(thread_id) && left->getSetup())
        {
            // left->lock(thread_id);
            left->lock(thread_id);
        }
        // do the main locking sequence
        (competitors)[side(thread_id)] = thread_id;     
        Fence();
        *tiebreaker = thread_id;
        Fence();
        spinners[thread_id-starting_thread_id] = 0;
        Fence();
        // Yang–Anderson's proof assumes sequential consistency. The doorway
        // STORES above are fenced, but the entry-protocol LOADS below must be
        // fenced too: on arm64 plain loads reorder freely (control
        // dependencies do not order loads), and two legal reorderings —
        // hoisting the ack check above the tiebreaker read, and re-reading a
        // stale tiebreaker after leaving the first wait loop — let both
        // sides skip/miss the ack handshake and deadlock (deterministic at
        // 5T pre-fix, flaky at 6T/12T).
        int rival = (competitors)[1 - side(thread_id)];
        Fence();
        if (rival != -1)
        { // there is someone competing
            if (*tiebreaker == static_cast<int>(thread_id))
            { // this thread_id either set the tiebreaker after the rival, or the rival has yet to set
                Fence();
                if (spinners[rival-starting_thread_id] == 0)
                {
                    spinners[rival-starting_thread_id] = 1; // tell the rival that we have updated the tiebreaker
                    Fence();
                }

                // Spin-then-yield (LockSpinWait) rather than raw spinning:
                // with more runnable threads than cores a raw spinner burns
                // the cycles its rival needs to reach its own store, stalling
                // whole runs (observed multi-second hangs at 12T on 8 cores).
                unsigned spins = 0;
                while (spinners[thread_id-starting_thread_id] == 0)
                {
                    LockSpinWait(spins);
                } // wait until rival either says they updated tiebreaker or they have finished crit section
                Fence();

                if (*tiebreaker == static_cast<int>(thread_id))
                { // we were later in setting tiebreaker
                    while (spinners[thread_id-starting_thread_id] !=2)
                    {
                        LockSpinWait(spins);
                    } // wait for rival to unlock

                }
            }
        }
        // Acquire side: critical-section accesses must not be reordered
        // above the grant observation; the lock has to provide this itself
        // rather than relying on callers fencing inside their CS.
        Fence();
    }

    void unlock(size_t thread_id)
    {                                      // unlike other locks, this takes a good amount of read/writes
        // Release side: the critical section's writes must be visible before
        // this side is seen as no longer competing / before the rival is
        // freed — without this the lock only worked for callers that fenced
        // inside their CS (as the benchmark does).
        Fence();
        (competitors)[side(thread_id)]=-1; // this side is no longer competing
        Fence();
        int rival = *tiebreaker;           // find out if you have a rival

        if (rival != static_cast<int>(thread_id))
        {                        // you have a competitor who is waiting
            spinners[rival-starting_thread_id] = 2; // free the competitor
            Fence();
        }
        if (side(thread_id) && right->getSetup())
        {
            right->unlock(thread_id);
        }
        if (!side(thread_id) && left->getSetup())
        {
            left->unlock(thread_id);
        }
    }
    void destroy() {
        free((void *)competitors);
        free((void *)tiebreaker);
        free((void *)spinners);
        if (left){left->destroy();}
        if (right){right->destroy();}
        free((void *)left);
        free((void *)right);
    };

private:
    int side(size_t thread_id)
    {
        if (thread_id < starting_thread_id + (num_threads / 2))
        {
            return 0;
        }
        return 1;
    }

    volatile size_t *spinners;
    volatile int *competitors;
    volatile int *tiebreaker;


    std::mutex mutex_left_;
    std::mutex mutex_right_;

    size_t num_threads;
    size_t starting_thread_id;
    YangMutexHelper *left;
    YangMutexHelper *right;
    bool setup;
};

class YangMutex : public virtual SoftwareMutex
{
public:
    void init(size_t num_threads) override
    {
        //maybe use something of smaller size
        this->num_threads = num_threads;
        helper_ = new YangMutexHelper();
        helper_->init(num_threads, 0);
    }

    void lock(size_t thread_id) override
    {
        helper_->lock(thread_id);
    }
    void unlock(size_t thread_id) override
    {
        helper_->unlock(thread_id);
    }
    void destroy() override
    {
        helper_->destroy();
    }

    std::string name() override { return "yang"; };

private:
    size_t num_threads;
    YangMutexHelper* helper_;
};
