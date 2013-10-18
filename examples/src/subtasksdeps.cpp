#include "superglue.hpp"
#include <iostream>
#include "option/instr_tasktiming.hpp"
#include "option/log_dag_data.hpp"

template<typename Options>
struct MyTaskBase : public TaskBaseDefault<Options> {
    size_t addedby;
    std::string getStyle() {
        switch(addedby) {
            case 0: return "[style=filled,fillcolor=red]";
            case 1: return "[style=filled,fillcolor=green]";
            case 2: return "[style=filled,fillcolor=blue]";
            case 3: return "[style=filled,fillcolor=magenta]";
            default:return "[style=filled,fillcolor=yellow]";
        }
    }
};

struct Options : public DefaultOptions<Options> {
    typedef Enable ThreadSafeSubmit; // this really should be the default
    typedef Enable Logging_DAG;
    typedef Enable TaskId;
    typedef Enable HandleId;
    typedef Enable TaskName;
    typedef Enable HandleName;
    typedef Enable Logging;
    typedef TaskExecutorTiming<Options> TaskExecutorInstrumentation;
    typedef MyTaskBase<Options> TaskBaseType;
};

void delay(Time::TimeUnit d) {
    Time::TimeUnit end = Time::getTime() + d;
    while (Time::getTime() < end);
}

struct gemm : public Task<Options> {
    size_t i, j;
    gemm(size_t addedby_, Handle<Options> &a, Handle<Options> &b, Handle<Options> &c,
         size_t i_, size_t j_) : i(i_), j(j_) {
        addedby = addedby_;
        registerAccess(ReadWriteAdd::read, &a);
        registerAccess(ReadWriteAdd::read, &b);
        registerAccess(ReadWriteAdd::add, &c);
    }
    void run() { delay(2000000); }
    std::string getName() {
        std::stringstream ss;
        ss << "gemm("<<i<<","<<j<<")";
        return ss.str();
    }
    
};
struct syrk : public Task<Options> {
    size_t i;
    syrk(size_t addedby_, Handle<Options> &a, Handle<Options> &b, size_t i_) : i(i_) {
        addedby = addedby_;
        registerAccess(ReadWriteAdd::read, &a);
        registerAccess(ReadWriteAdd::add, &b);
    }
    void run() { delay(1000000); }
    std::string getName() {
        std::stringstream ss;
        ss << "syrk("<<i<<","<<i<<")";
        return ss.str();
    }
};
struct potrf : public Task<Options> {
    size_t i;
    potrf(size_t addedby_, Handle<Options> &a, size_t i_) : i(i_) {
        addedby = addedby_;
        registerAccess(ReadWriteAdd::write, &a);
    }
    void run() { delay(1000000); }
    std::string getName() {
        std::stringstream ss;
        ss << "potrf("<<i<<","<<i<<")";
        return ss.str();
    }
};
struct trsm : public Task<Options> {
    size_t i, j;
    trsm(size_t addedby_, Handle<Options> &a, Handle<Options> &b,
         size_t i_, size_t j_) : i(i_), j(j_) {
        addedby = addedby_;
        registerAccess(ReadWriteAdd::read, &a);
        registerAccess(ReadWriteAdd::write, &b);
    }
    void run() { delay(1000000); }
    std::string getName() {
        std::stringstream ss;
        ss << "trsm("<<i<<","<<j<<")";
        return ss.str();
    }
};

// <HIDE INSIDE TASK LIBRARY>
struct propagate : public Task<Options> {
    size_t index, i, j;

    propagate(size_t addedby_, Handle<Options> *src, Handle<Options> *dst, size_t index_,
              size_t i_, size_t j_)
    : index(index_), i(i_), j(j_) {
        addedby = addedby_;
        registerAccess(ReadWriteAdd::read, src);
        addAccess(ReadWriteAdd::write, dst, 0);
    }
    void run() {}
    std::string getName() {
        std::stringstream ss;
        ss << "prop(" << i << "," << j << ")_from_" << index << "_to_" << index+1;
        return ss.str();
    }
};
// </HIDE INSIDE TASK LIBRARY>


struct bigtask : public Task<Options> {
    ThreadManager<Options> &tm;
    size_t numBlocks;
    size_t index;
    Handle<Options> *h;

// <HIDE INSIDE TASK LIBRARY>
    void promise(Handle<Options> *h) {
        // updates the next-available-version of the handle
        h->schedule(ReadWriteAdd::write);
    }
// </HIDE INSIDE TASK LIBRARY>

    bigtask(ThreadManager<Options> &tm_,
            size_t numBlocks_, size_t index_, Handle<Options> *h_)
    : tm(tm_), numBlocks(numBlocks_), index(index_), h(h_) {

        // make a promise that someone will update these handles at some point
        // in the future, but dont automatically update them when this task is
        // finished (since this task only submits tasks)
        //
        // this is run when the task is created, that is, on the main thread.

        for (size_t i = index+1; i < numBlocks; ++i)
            for (size_t j = index+1; j <= i; ++j)
                promise(&h[ (index+1)*numBlocks*numBlocks + i*numBlocks + j]);

        // in this solution i dont have major and minor version numbers like
        //   "1.1", "1.2", ..., "2.1", "2.2", ...
        // but instead have a new handle for each "major" version, and
        // let the minor version be the ordinary handle version number.
        //
        // if we include all version numbers in the handle, we could
        // automatically make promise() add tasks that read all handles
        // for which promises were made (this is then made AFTER all subtasks
        // are created, so we will know which handle version to depend on)
        // and propagate the major version.
    }

    void run() {
        Handle<Options> *A( &h[index*numBlocks*numBlocks] );

        const size_t k = index;

        // cholesky factorization of A[k,k]

        tm.submit(new potrf(index, A[k*numBlocks + k], k));

        // update panel

        for (size_t m = k+1; m < numBlocks; ++m) {
            // A[m,k] <- A[m,k] = X * (A[k,k])^t
            tm.submit(new trsm(index, A[k*numBlocks + k],
                               A[m*numBlocks + k], m, k));
        }

        // update submatrix

        for (size_t n = k+1; n < numBlocks; ++n) {

            // A[n,n] = A[n,n] - A[n,k] * (A[n,n])^t
            tm.submit(new syrk(index, A[n*numBlocks + k],
                               A[n*numBlocks + n], n));

            for (size_t m = n+1; m < numBlocks; ++m) {
                // A[m,n] = A[m,n] - A[m,k] * (A[n,k])^t
                tm.submit(new gemm(index, A[m*numBlocks + k],
                                   A[n*numBlocks + k],
                                   A[m*numBlocks + n], m, n));
            }
        }

// <HIDE INSIDE TASK LIBRARY>
        // keep our promise: create tasks that propagate the handle versions.
        //
        // these tasks could be added automatically by the task library if we
        // allow handles to have nested version numbers.
        for (size_t i = index+1; i < numBlocks; ++i) {
            for (size_t j = index+1; j <= i; ++j) {
                tm.submit(new propagate(index,&A[i*numBlocks + j],
                                        &h[(index+1)*numBlocks*numBlocks + i*numBlocks + j],
                                        index, i, j));

            }
        }
// </HIDE INSIDE TASK LIBRARY>
    }

    std::string getName() {
        std::stringstream ss;
        ss << "bigtask(" << index << ")";
        return ss.str();
    }
};

int main() {

    const size_t numBlocks = 3;

    // create (numBlocks+1) x (numBlocks) x (numBlocks) handles
    Handle<Options> *A = new Handle<Options>[(1+numBlocks)*numBlocks*numBlocks];

    // this is just for debugging: name the handles
    for (size_t k = 0; k < numBlocks+1; ++k) {
        for (size_t i = 0; i < numBlocks; ++i) {
            for (size_t j = 0; j < numBlocks; ++j) {
                std::stringstream ss;
                ss<<k<<":("<<i<<","<<j<<")";
                A[k*numBlocks*numBlocks + i*numBlocks + j].setName(ss.str().c_str());
            }
        }
    }

    ThreadManager<Options> tm;

    // create bigtasks
    for (size_t i = 0; i < numBlocks; ++i)
        tm.submit(new bigtask(tm, numBlocks, i, A));
    tm.barrier();

    Log<Options>::dump("log.log");
    Log_DAG_data<Options>::dump("cholesky_data.dot");

    return 0;
}