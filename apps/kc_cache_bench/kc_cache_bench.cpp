
#include <time.h>
#include <string.h>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include <string>

#include "kc_cache_bench.hpp"
#include "bench_utils.hpp"

#include "../../lib/kyotocabinet/kccachedb.h"
#include "../../lib/kyotocabinet/cmdcommon.h"

// TODO: check that all db commands are using our lock, not pthread!!

const char *g_progname = "kc_cache_bench"; // program name
uint32_t g_randseed;                       // random seed
int64_t g_memusage;                        // memory usage

// print the error message of a database
static void dberrprint(kc::BasicDB *db, int32_t line, const char *func)
{
    const kc::BasicDB::Error &err = db->error();
    oprintf("%s: %d: %s: %s: %d: %s: %s\n",
            g_progname, line, func, db->path().c_str(), err.code(), err.name(), err.message());
}

std::shared_ptr<std::atomic<bool>> start_flag = std::make_shared<std::atomic<bool>>(false);
std::shared_ptr<std::atomic<bool>> end_flag = std::make_shared<std::atomic<bool>>(false);
std::vector<per_thread_args> thread_args = std::vector<per_thread_args>();
SoftwareMutex *dbw_lock;
SoftwareMutex *dbr_lock;

void kc_cache_bench::run(
    int num_threads,
    double run_time,
    int capsiz,
    bool csv,
    bool rusage,
    bool no_output)
{

    dbw_lock->init(num_threads+1);
    dbr_lock->init(num_threads+1);

    for (int i = 0; i < num_threads; ++i)
    {
        thread_args.push_back({});
        thread_args[i].thread_id = i;
        thread_args[i].stats.run_time = run_time;
        thread_args[i].stats.num_iterations = 0;
        thread_args[i].start_flag = start_flag;
        thread_args[i].end_flag = end_flag;
    }

    g_randseed = (uint32_t)(kc::time() * 1000);
    mysrand(g_randseed);
    thr_id=num_threads;
    g_memusage = memusage();
    kc::setstdiobin();
    bool err = false;
    kc::CacheDB db;
    db.tune_logger(stdlogger(g_progname, &std::cout),
                   kc::BasicDB::Logger::WARN | kc::BasicDB::Logger::ERROR);
    if (capsiz > 0)
        db.cap_size(capsiz);

    uint32_t omode = kc::CacheDB::OWRITER | kc::CacheDB::OCREATE;
    omode |= kc::CacheDB::OTRUNCATE;

    if (!db.open("*", omode))
    {
        dberrprint(&db, __LINE__, "DB::open");
        err = true;
    }
    // class ThreadWicked : public kc::Thread {
    // public:
    // void setparams(int32_t id, kc::CacheDB* db, int32_t thnum,
    //                 const char* lbuf) {
    //     id_ = id;
    //     db_ = db;
    //     thnum_ = thnum;
    //     lbuf_ = lbuf;
    //     err_ = false;
    // }
    // bool error() {
    //     return err_;
    // }
    // void run() {

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    char lbuf[RECBUFSIZL];
    std::memset(lbuf, '*', sizeof(lbuf));

    Fence();

    for (int id_ = 0; id_ < num_threads; id_++)
    {
        threads.emplace_back([&, id_]()
                             {

        thr_id=id_;
        thread_args[id_].stats.thread_id=id_;

        kc::CacheDB* db_ = &db;
        int32_t thnum_ = num_threads;
        const char* lbuf_ = lbuf;
        bool err_ = false;
        kc::DB::Cursor* cur = db_->cursor();
        int64_t range = run_time*700000 * thnum_ / 2;
        while (!*start_flag){}
        while (!*end_flag && !err_){
        thread_args[id_].stats.num_iterations++;

        char kbuf[RECBUFSIZ];
        size_t ksiz = std::sprintf(kbuf, "%lld", (long long)(myrand(range) + 1));
        // if (myrand(1000) == 0) {
        //     ksiz = myrand(RECBUFSIZ) + 1;
        //     if (myrand(2) == 0) {
        //     for (size_t j = 0; j < ksiz; j++) {
        //         kbuf[j] = j;
        //     }
        //     } else {
        //     for (size_t j = 0; j < ksiz; j++) {
        //         kbuf[j] = myrand(256);
        //     }
        //     }
        // }
        const char* vbuf = kbuf;
        size_t vsiz = ksiz;
        // if (myrand(10) == 0) {
        //     vbuf = lbuf_;
        //     vsiz = myrand(RECBUFSIZL) / (myrand(5) + 1);
        // }
        do {
            switch (myrand(9)) {
            // case 0: {
            //     if (!db_->set(kbuf, ksiz, vbuf, vsiz)) {
            //     dberrprint(db_, __LINE__, "DB::set");
            //     err_ = true;
            //     }
            //     break;
            // }
            // case 1: {
            //     if (!db_->add(kbuf, ksiz, vbuf, vsiz) &&
            //         db_->error() != kc::BasicDB::Error::DUPREC) {
            //     dberrprint(db_, __LINE__, "DB::add");
            //     err_ = true;
            //     }
            //     break;
            // }
            // case 2: {
            //     if (!db_->replace(kbuf, ksiz, vbuf, vsiz) &&
            //         db_->error() != kc::BasicDB::Error::NOREC) {
            //     dberrprint(db_, __LINE__, "DB::replace");
            //     err_ = true;
            //     }
            //     break;
            // }
            // case 3: {
            //     if (!db_->append(kbuf, ksiz, vbuf, vsiz)) {
            //     dberrprint(db_, __LINE__, "DB::append");
            //     err_ = true;
            //     }
            //     break;
            // }
            // case 4: {
            //     if (myrand(2) == 0) {
            //     int64_t num = myrand(500000);
            //     int64_t orig = myrand(10) == 0 ? kc::INT64MIN : myrand(500000);
            //     if (myrand(10) == 0) orig = orig == kc::INT64MIN ? kc::INT64MAX : -orig;
            //     if (db_->increment(kbuf, ksiz, num, orig) == kc::INT64MIN &&
            //         db_->error() != kc::BasicDB::Error::LOGIC) {
            //         dberrprint(db_, __LINE__, "DB::increment");
            //         err_ = true;
            //     }
            //     } else {
            //     double num = myrand(500000 * 10) / (myrand(500000) + 1.0);
            //     double orig = myrand(10) == 0 ? -kc::inf() : myrand(500000);
            //     if (myrand(10) == 0) orig = -orig;
            //     if (kc::chknan(db_->increment_double(kbuf, ksiz, num, orig)) &&
            //         db_->error() != kc::BasicDB::Error::LOGIC) {
            //         dberrprint(db_, __LINE__, "DB::increment_double");
            //         err_ = true;
            //     }
            //     }
            //     break;
            // }
            // case 5: {
            //     if (!db_->cas(kbuf, ksiz, kbuf, ksiz, vbuf, vsiz) &&
            //         db_->error() != kc::BasicDB::Error::LOGIC) {
            //     dberrprint(db_, __LINE__, "DB::cas");
            //     err_ = true;
            //     }
            //     break;
            // }
            // case 6: {
            //     if (!db_->remove(kbuf, ksiz) &&
            //         db_->error() != kc::BasicDB::Error::NOREC) {
            //     dberrprint(db_, __LINE__, "DB::remove");
            //     err_ = true;
            //     }
            //     break;
            // }
            // case 7: {
            //     if (myrand(2) == 0) {
            //     if (db_->check(kbuf, ksiz) < 0 && db_->error() != kc::BasicDB::Error::NOREC) {
            //         dberrprint(db_, __LINE__, "DB::check");
            //         err_ = true;
            //     }
            //     } else {
            //     size_t rsiz;
            //     char* rbuf = db_->seize(kbuf, ksiz, &rsiz);
            //     if (rbuf) {
            //         delete[] rbuf;
            //     } else if (db_->error() != kc::BasicDB::Error::NOREC) {
            //         dberrprint(db_, __LINE__, "DB::seize");
            //         err_ = true;
            //     }
            //     }
            //     break;
            // }
            // case 8: {
            //     class VisitorImpl : public kc::DB::Visitor {
            //     public:
            //         explicit VisitorImpl(const char* lbuf) : lbuf_(lbuf) {}
            //     private:
            //         const char* visit_full(const char* kbuf, size_t ksiz,
            //                             const char* vbuf, size_t vsiz, size_t* sp) {
            //         const char* rv = NOP;
            //         switch (myrand(3)) {
            //             case 0: {
            //             rv = lbuf_;
            //             *sp = myrand(RECBUFSIZL) / (myrand(5) + 1);
            //             break;
            //             }
            //             case 1: {
            //             rv = REMOVE;
            //             break;
            //             }
            //         }
            //         return rv;
            //         }
            //         const char* lbuf_;
            //     } visitor(lbuf_);
            //     if (!cur->accept(&visitor, true, myrand(2) == 0) &&
            //         db_->error() != kc::BasicDB::Error::NOREC) {
            //         dberrprint(db_, __LINE__, "Cursor::accept");
            //         err_ = true;
            //     }
            //     if (myrand(5) > 0 && !cur->step() &&
            //         db_->error() != kc::BasicDB::Error::NOREC) {
            //         dberrprint(db_, __LINE__, "Cursor::step");
            //         err_ = true;
            //     }
            //     break;
            // }
            default: {
                size_t rsiz;
                char* rbuf = db_->get(kbuf, ksiz, &rsiz);
                if (rbuf) {
                delete[] rbuf;
                } else if (db_->error() != kc::BasicDB::Error::NOREC) {
                dberrprint(db_, __LINE__, "DB::get");
                err_ = true;
                }
                break;
            }
            }
        } while (myrand(100) == 0);
        // if (myrand(100) == 0) {
        //     int32_t jnum = myrand(10);
        //     switch (myrand(4)) {
        //     case 0: {
        //         std::map<std::string, std::string> recs;
        //         for (int32_t j = 0; j < jnum; j++) {
        //         char jbuf[RECBUFSIZ];
        //         size_t jsiz = std::sprintf(jbuf, "%lld", (long long)(myrand(range) + 1));
        //         recs[std::string(jbuf, jsiz)] = std::string(kbuf, ksiz);
        //         }
        //         if (db_->set_bulk(recs, myrand(4)) != (int64_t)recs.size()) {
        //         dberrprint(db_, __LINE__, "DB::set_bulk");
        //         err_ = true;
        //         }
        //         break;
        //     }
        //     case 1: {
        //         std::vector<std::string> keys;
        //         for (int32_t j = 0; j < jnum; j++) {
        //         char jbuf[RECBUFSIZ];
        //         size_t jsiz = std::sprintf(jbuf, "%lld", (long long)(myrand(range) + 1));
        //         keys.push_back(std::string(jbuf, jsiz));
        //         }
        //         if (db_->remove_bulk(keys, myrand(4)) < 0) {
        //         dberrprint(db_, __LINE__, "DB::remove_bulk");
        //         err_ = true;
        //         }
        //         break;
        //     }
        //     default: {
        //         std::vector<std::string> keys;
        //         for (int32_t j = 0; j < jnum; j++) {
        //         char jbuf[RECBUFSIZ];
        //         size_t jsiz = std::sprintf(jbuf, "%lld", (long long)(myrand(range) + 1));
        //         keys.push_back(std::string(jbuf, jsiz));
        //         }
        //         std::map<std::string, std::string> recs;
        //         if (db_->get_bulk(keys, &recs, myrand(4)) < 0) {
        //         dberrprint(db_, __LINE__, "DB::get_bulk");
        //         err_ = true;
        //         }
        //         break;
        //     }
        //     }
        //     if (!db_->switch_rotation(myrand(4) > 0)) {
        //     dberrprint(db_, __LINE__, "DB::switch_rotation");
        //     err_ = true;
        //     }
        // }
        }
        if (err_){thread_args[id_].stats.num_iterations=-100000000000;}
        delete cur; });
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(.01)); //give time to setup //TODO: make less
    *start_flag = true;
    Fence();
    std::this_thread::sleep_for(std::chrono::duration<double>(run_time));
    *end_flag = true;
    Fence();

    for (int32_t i = 0; i < num_threads; i++)
    {
        if (threads[i].joinable())
            threads[i].join();
    }
    if (!db.close())
    {
        dberrprint(&db, __LINE__, "DB::close");
        err = true;
    }

    if (rusage && !no_output)
    {
        record_rusage(csv);
    }

    // dbw_lock->destroy();
    // dbr_lock->destroy();

    if (!no_output && !rusage)
    {
        for (auto &targs : thread_args)
        {
            report_thread_latency(&targs.stats, csv, true);
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        fprintf(stderr,
                "Usage: %s <mutex_name> <num_threads> <run_time_s> <capsize>"
                "[--csv]\n",
                argv[0]);
        return 1;
    }

    const char *mutex_name = argv[1];
    int num_threads = atoi(argv[2]);
    double run_time_sec = atof(argv[3]);
    double capsize = atof(argv[4]);

    bool csv = false;
    bool no_output = false;
    bool rusage_ = false;

    for (int i = 5; i < argc; ++i)
    {
        if (strcmp(argv[i], "--csv") == 0)
        {
            csv = true;
        }
        else if (strcmp(argv[i], "--rusage") == 0)
        {
            rusage_ = true;
        }
        else if (strcmp(argv[i], "--no-output") == 0)
        {
            no_output = true;
        }
        else
        {
            fprintf(stderr, "Unrecognized flag: %s\n", argv[i]);
            return 1;
        }
    }

    dbw_lock = get_mutex(mutex_name, num_threads);
    dbr_lock = get_mutex(mutex_name, num_threads);
    if (!dbw_lock || !dbr_lock)
    {

        return 1;
    }

    kc_cache_bench::run(
        num_threads,
        run_time_sec,
        capsize,
        csv,
        rusage_,
        no_output);

    return 0;
}
