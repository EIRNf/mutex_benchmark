#include "lock.hpp"
#include <stdexcept>
#include <mutex>

class YangMutexHelper
{
public:
    bool getSetup(){
        return setup;
    }

    void init(size_t num_threads, size_t starting_thread_id)
    {
        this->spinners = (volatile size_t *)malloc(sizeof(size_t) * num_threads);

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
        int rival = (competitors)[1 - side(thread_id)];
        if (rival != -1)
        { // there is someone competing
            if (*tiebreaker == thread_id)
            { // this thread_id either set the tiebreaker after the rival, or the rival has yet to set
                if (spinners[rival-starting_thread_id] == 0)
                {
                    spinners[rival-starting_thread_id] = 1; // tell the rival that we have updated the tiebreaker
                    Fence();
                }

                while (spinners[thread_id-starting_thread_id] == 0)
                {
                } // wait until rival either says they updated tiebreaker or they have finished crit section

                if (*tiebreaker == thread_id)
                { // we were later in setting tiebreaker
                    while (spinners[thread_id-starting_thread_id] !=2)
                    {
                        
                    } // wait for rival to unlock

                }
            }
        }
    }

    void unlock(size_t thread_id)
    {                                      // unlike other locks, this takes a good amount of read/writes
        (competitors)[side(thread_id)]=-1; // this side is no longer competing
        Fence();
        int rival = *tiebreaker;           // find out if you have a rival

        if (rival != thread_id)
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
        if (thread_id < starting_thread_id + (int)(num_threads / 2))
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

    std::string name() { return "yang"; };

private:
    size_t num_threads;
    YangMutexHelper* helper_;
};
