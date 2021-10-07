#ifndef GREENLET_THREAD_STATE_HPP
#define GREENLET_THREAD_STATE_HPP

#include <stdexcept>
#include "greenlet_internal.hpp"
#include "greenlet_thread_support.hpp"


namespace greenlet {
/**
 * Thread-local state of greenlets.
 *
 * Each native thread will get exactly one of these objects,
 * autmatically accessed through the best available thread-local
 * mechanism the compiler supports (``thread_local`` for C++11
 * compilers or ``__thread``/``declspec(thread)`` for older GCC/clang
 * or MSVC, respectively.)
 *
 * Previously, we kept thread-local state mostly in a bunch of
 * ``static volatile`` variables in the main greenlet file.. This had
 * the problem of requiring extra checks, loops, and great care
 * accessing these variables if we potentially invoked any Python code
 * that could relaese the GIL, because the state could change out from
 * under us. Making the variables thread-local solves this problem.
 *
 * When we detected that a greenlet API accessing the current greenlet
 * was invoked from a different thread than the greenlet belonged to,
 * we stored a reference to the greenlet in the Python thread
 * dictionary for the thread the greenlet belonged to. This could lead
 * to memory leaks if the thread then exited (because of a reference
 * cycle, as greenlets referred to the thread dictionary, and deleting
 * non-current greenlets leaked their frame plus perhaps arguments on
 * the C stack). If a thread exited while still having running
 * greenlet objects (perhaps that had just switched back to the main
 * greenlet), and did not invoke one of the greenlet APIs *in that
 * thread, immediately before it exited, without some other thread
 * then being invoked*, such a leak was guaranteed.
 *
 * This can be partly solved by using compiler thread-local variables
 * instead of the Python thread dictionary, thus avoiding a cycle.
 *
 * To fully solve this problem, we need a reliable way to know that a
 * thread is done and we should clean up the main greenlet. On POSIX,
 * we can use the destructor function of ``pthread_key_create``, but
 * there's nothing similar on Windows; a C++11 thread local object
 * reliably invokes its destructor when the thread it belongs to exits
 * (non-C++11 compilers offer ``__thread`` or ``declspec(thread)`` to
 * create thread-local variables, but they can't hold C++ objects that
 * invoke destructors; the C++11 version is the most portable solution
 * I found). When the thread exits, we can drop references and
 * otherwise manipulate greenlets and frames that we know can no
 * longer be switched to. For compilers that don't support C++11
 * thread locals, we have a solution that uses the python thread
 * dictionary, though it may not collect everything as promptly as
 * other compilers do, if some other library is using the thread
 * dictionary and has a cycle or extra reference.
 *
 * There are two small wrinkles. The first is that when the thread
 * exits, it is too late to actually invoke Python APIs: the Python
 * thread state is gone, and the GIL is released. To solve *this*
 * problem, our destructor uses ``Py_AddPendingCall`` to transfer the
 * destruction work to the main thread. (This is not an issue for the
 * dictionary solution.)
 *
 * The second is that once the thread exits, the thread local object
 * is invalid and we can't even access a pointer to it, so we can't
 * pass it to ``Py_AddPendingCall``. This is handled by actually using
 * a second object that's thread local (ThreadStateCreator) and having
 * it dynamically allocate this object so it can live until the
 * pending call runs.
 */
class ThreadState {
private:
    // As of commit 08ad1dd7012b101db953f492e0021fb08634afad
    // this class needed 56 bytes in o Py_DEBUG build
    // on 64-bit macOS 11.
    // Adding the vector takes us up to 80 bytes ()

    /* Strong reference to the main greenlet */
    PyMainGreenlet* main_greenlet_s;

    /* Strong reference to the current greenlet. */
    PyGreenlet* current_greenlet_s;
    /*  Weak reference to the switching-to greenlet during the slp
        switch */
    PyGreenlet* target_greenlet_w;
    /* NULL if error, otherwise weak refernce to args tuple to pass around during slp switch */
    PyObject* switch_args_w;
    PyObject* switch_kwargs_w;

    /* Strong reference to the switching from greenlet after the switch */
    PyGreenlet* origin_greenlet_s;

    /* Strong reference to the trace function, if any. */
    PyObject* tracefunc_s;

    /* A vector of PyGreenlet pointers representing things that need
       deleted when this thread is running. The vector owns the
       references, but you need to manually INCREF/DECREF as you use
       them. */
    greenlet::g_deleteme_t deleteme;

    /* Used internally in ``g_switchstack()`` when
       GREENLET_USE_CFRAME is true. */
    int _switchstack_use_tracing;

    G_NO_COPIES_OF_CLS(ThreadState);

public:

    ThreadState() :
        main_greenlet_s(0),
        current_greenlet_s(0),
        target_greenlet_w(0),
        switch_args_w(0),
        switch_kwargs_w(0),
        origin_greenlet_s(0),
        tracefunc_s(0),
        _switchstack_use_tracing(0)
    {
    };

    inline int switchstack_use_tracing()
    {
        return this->_switchstack_use_tracing;
    }

    inline void switchstack_use_tracing(int new_value)
    {
        this->_switchstack_use_tracing = new_value;
    }

    inline bool has_main_greenlet()
    {
        return this->main_greenlet_s != nullptr;
    }

    inline PyMainGreenlet* borrow_main_greenlet()
    {
        if (!this->main_greenlet_s) {
            assert(!this->current_greenlet_s);
            // steal the reference
            this->main_greenlet_s = green_create_main();
            if (this->main_greenlet_s) {
                this->main_greenlet_s->thread_state = this;
                this->set_current((PyGreenlet*)this->main_greenlet_s);
                assert(Py_REFCNT(this->main_greenlet_s) == 3);
            }
        }
        return this->main_greenlet_s;
    };

    inline PyMainGreenlet* get_main_greenlet()
    {
        PyMainGreenlet* g = this->borrow_main_greenlet();
        Py_XINCREF(g);
        return g;
    }

    inline PyGreenlet* borrow_current() const
    {
        return this->current_greenlet_s;
    };

    inline PyGreenlet* get_current()
    {
        Py_INCREF(this->current_greenlet_s);
        return this->current_greenlet_s;
    };

    inline bool is_current(const void* obj) const
    {
        return this->current_greenlet_s == obj;
    }

    inline PyGreenlet* get_or_establish_current()
    {
        if (!this->ensure_current_greenlet()) {
            return NULL;
        }
        return this->get_current();
    }

    inline PyObject* get_or_establish_current_object()
    {
        return reinterpret_cast<PyObject*>(this->get_or_establish_current());
    };

    inline void set_current(PyGreenlet* new_greenlet)
    {
        Py_INCREF(new_greenlet);
        Py_XDECREF(this->current_greenlet_s);
        this->current_greenlet_s = new_greenlet;
    };

    /**
     * Steals the reference to new_target, and drops the reference to
     * the current greenlet. You should either decref an existing
     * pointer to the current reference, or steal it somewhere.
     * XXX: Deprecated, confusing.
     */
    inline void release_ownership_of_current_and_steal_new_current(PyGreenlet* new_target)
    {
        this->current_greenlet_s = new_target;
    }

private:
    /**
     * Deref and remove the greenlets from the deleteme list. Must be
     * holding the GIL.
     *
     * If *murder* is true, then we must be called from a different
     * thread than the one that these greenlets were running in.
     * In that case, if the greenlet was actually running, we destroy
     * the frame reference and otherwise make it appear dead before
     * proceeding; otherwise, we would try (and fail) to raise an
     * exception in it and wind up right back in this list.
     */
    inline void clear_deleteme_list(const bool murder=false)
    {
        if (!this->deleteme.empty()) {
            g_deleteme_t copy = this->deleteme;
            this->deleteme.clear(); // in case things come back on the list
            for(greenlet::g_deleteme_t::iterator it = copy.begin(), end = copy.end();
                it != end;
                ++it ) {
                PyGreenlet* to_del = *it;
                fprintf(stderr, "Deleteme: Decref %p; active? %d Refcount: %ld\n",
                        to_del, PyGreenlet_ACTIVE(to_del), Py_REFCNT(to_del));
                if (murder) {
                    // Force each greenlet to appear dead; we can't raise an
                    // exception into it anymore anyway.

                    Py_CLEAR(to_del->main_greenlet_s);
                    fprintf(stderr, "\tMurdering %p; active? %d\n", to_del, PyGreenlet_ACTIVE(to_del));
                    if (PyGreenlet_ACTIVE(to_del)) {
                        assert(to_del->top_frame);
                        to_del->stack_start = NULL;
                        assert(!PyGreenlet_ACTIVE(to_del));

                        // We're holding a borrowed reference to the last
                        // frome we executed. Since we borrowed it, the
                        // normal traversal, clear, and dealloc functions
                        // ignore it, meaning it leaks. (The thread state
                        // object can't find it to clear it when that's
                        // deallocated either, because by definition if we
                        // got an object on this list, it wasn't
                        // running and the thread state doesn't have
                        // this frame.)
                        // So here, we *do* clear it.
                        Py_CLEAR(to_del->top_frame);
                    }
                }

                // The only reference to these greenlets should be in
                // this list, decreffing them should let them be
                // deleted again, triggering calls to green_dealloc()
                // in the correct thread (if we're not murdering).
                // This may run arbitrary Python code and switch
                // threads.
                Py_ssize_t refs = Py_REFCNT(to_del);
                Py_DECREF(to_del);
                if (refs > 1) {
                    fprintf(stderr, "\tResurrected? %p before=%ld after=%ld\n",
                            to_del, refs, Py_REFCNT(to_del));
                }
                if (PyErr_Occurred()) {
                    PyErr_WriteUnraisable(nullptr);
                    PyErr_Clear();
                }
            }
        }
        if (!this->deleteme.empty()) {
            fprintf(stderr, "During deletion got more items: %ld\n", this->deleteme.size());
        }
    }

public:
    inline bool ensure_current_greenlet()
    {
        // called from STATE_OK. That macro previously
        // expanded to a function that checked ts_delkey,
        // which, if it needs cleaning, can result in greenlet
        // switches or thread switches.

        if (!this->current_greenlet_s) {
            assert(!this->main_greenlet_s);
            if (!this->borrow_main_greenlet()) {
                return false;
            }
        }
        /* green_dealloc() cannot delete greenlets from other threads, so
           it stores them in the thread dict; delete them now. */


        assert(this->current_greenlet_s->main_greenlet_s == this->main_greenlet_s);
        assert(this->main_greenlet_s->super.main_greenlet_s == this->main_greenlet_s);
        this->clear_deleteme_list();
        return true;
    };

    inline PyGreenlet* borrow_target() const
    {
        return this->target_greenlet_w;
    };

    inline void wref_target(PyGreenlet* target)
    {
        this->target_greenlet_w = target;
    };

    inline void wref_switch_args_kwargs(PyObject* args, PyObject* kwargs)
    {
        this->switch_args_w = args;
        this->switch_kwargs_w = kwargs;
    };

    inline PyObject* borrow_switch_args()
    {
        return this->switch_args_w;
    };

    inline PyObject* borrow_switch_kwargs()
    {
        return this->switch_kwargs_w;
    };

    inline PyGreenlet* borrow_origin() const
    {
        return this->origin_greenlet_s;
    };

    inline PyGreenlet* release_ownership_of_origin()
    {
        PyGreenlet* result = this->origin_greenlet_s;
        this->origin_greenlet_s = NULL;
        return result;
    }

    inline void steal_ownership_as_origin(PyGreenlet* new_origin)
    {
        assert(this->origin_greenlet_s == NULL);
        assert(new_origin != NULL);
        this->origin_greenlet_s = new_origin;
    };

    /**
     * Returns a new reference, or NULL.
     */
    inline PyObject* get_tracefunc() const
    {
        Py_XINCREF(this->tracefunc_s);
        return tracefunc_s;
    };

    inline void del_tracefunc()
    {
        Py_CLEAR(this->tracefunc_s);
    }

    inline void set_tracefunc(PyObject* tracefunc)
    {
        assert(tracefunc);
        Py_INCREF(tracefunc);
        Py_CLEAR(this->tracefunc_s);
        this->tracefunc_s = tracefunc;
    }

    /**
     * Given a reference to a greenlet that some other thread
     * attempted to delete (has a refcount of 0) store it for later
     * deletion when the thread this state belongs to is current.
     */
    inline void delete_when_thread_running(PyGreenlet* to_del)
    {
        Py_INCREF(to_del);
        this->deleteme.push_back(to_del);
    }

    ~ThreadState()
    {
        if (!PyInterpreterState_Head()) {
            // We shouldn't get here (our callers protect us)
            // but if we do, all we can do is bail early.
            return;
        }

        // We should not have an "origin" greenlet; that only exists
        // for the temporary time during a switch, which should not
        // be in progress as the thread dies.
        assert(this->origin_greenlet_s == nullptr);

        Py_CLEAR(this->tracefunc_s);

        // Forcibly GC as much as we can.
        this->clear_deleteme_list(true);

        // The pending call did this.
        assert(this->main_greenlet_s->thread_state == NULL);

        // If the main greenlet is the current greenlet,
        // then we "fell off the end" and the thread died.
        // It's possible that there is some other greenlet that
        // switched to us, leaving a reference to the main greenlet
        // on the stack, somewhere uncollectable. Try to detect that.
        if (this->current_greenlet_s == (void*)this->main_greenlet_s && this->current_greenlet_s) {
            assert(PyGreenlet_MAIN(this->main_greenlet_s));
            assert(!this->current_greenlet_s->top_frame);
            assert(this->main_greenlet_s->super.main_greenlet_s == this->main_greenlet_s);
            // Break a cycle we know about, the self reference
            // the main greenlet keeps.
            Py_CLEAR(this->main_greenlet_s->super.main_greenlet_s);
            // Drop one reference we hold.
            Py_CLEAR(this->current_greenlet_s);
            // Only our reference to the main greenlet should be left,
            // But hold onto the pointer in case we need to do extra cleanup.
            PyMainGreenlet* old_main_greenlet = this->main_greenlet_s;
            Py_ssize_t cnt = Py_REFCNT(this->main_greenlet_s);
            Py_CLEAR(this->main_greenlet_s);
            if (cnt == 2 && Py_REFCNT(old_main_greenlet) == 1) {
                // Highly likely that the reference is somewhere on
                // the stack, not reachable by GC. Verify.
                // XXX: This is O(n) in the total number of objects.
                PyObject* gc = PyImport_ImportModule("gc");
                PyObject* get_referrers = NULL;
                PyObject* refs = NULL;
                if (gc) {
                    get_referrers = PyObject_GetAttrString(gc, "get_referrers");
                    assert(get_referrers);
                    refs = PyObject_CallFunctionObjArgs(get_referrers, old_main_greenlet, NULL);
                    if (refs && !PyList_GET_SIZE(refs)) {
                        // We found nothing! So we left a dangling
                        // reference: Probably the last thing some
                        // other greenlet did was call
                        // 'getcurrent().parent.switch()' to switch
                        // back to us. Clean it up. This will be the
                        // case on CPython 3.7 and newer, as they use
                        // an internal calling convertion that avoids
                        // creating method objects and storing them on
                        // the stack.
                        Py_DECREF(old_main_greenlet);
                    }
                    else if (refs
                             && PyList_GET_SIZE(refs) == 1
                             && PyCFunction_Check(PyList_GET_ITEM(refs, 0))
                             && Py_REFCNT(PyList_GET_ITEM(refs, 0)) == 2) {
                        // Ok, we found a C method that refers to the
                        // main greenlet, and its only referenced
                        // twice, once in the list we just created,
                        // once from...somewhere else. If we can't
                        // find where else, then this is a leak.
                        // This happens in older versions of CPython
                        // that create a bound method object somewhere
                        // on the stack that we'll never get back to.
                        if (PyCFunction_GetFunction(PyList_GET_ITEM(refs, 0)) == (PyCFunction)green_switch) {
                            PyObject* function_w = PyList_GET_ITEM(refs, 0);
                            Py_DECREF(refs);
                            // back to one reference. Can *it* be
                            // found?
                            refs = PyObject_CallFunctionObjArgs(get_referrers, function_w, NULL);
                            if (refs && !PyList_GET_SIZE(refs)) {
                                // Nope, it can't be found so it won't
                                // ever be GC'd. Drop it.
                                Py_CLEAR(function_w);
                            }
                        }
                    }
                }
                Py_XDECREF(gc);
                Py_XDECREF(refs);
                Py_XDECREF(get_referrers);
            }
        }

        // We need to make sure this greenlet appears to be dead,
        // because otherwise deallocing it would fail to raise an
        // exception in it (the thread is dead) and put it back in our
        // deleteme list.
        if (this->current_greenlet_s) {
            PyGreenlet* g = this->current_greenlet_s;
            assert(!g->top_frame);
            Py_CLEAR(g->main_greenlet_s);
            Py_DECREF(this->current_greenlet_s);
        }

        if (this->main_greenlet_s) {
            // Couldn't have been the main greenlet that was running
            // when the thread exited (because we already cleared this
            // pointer if it was). This shouldn't be possible?
            assert(PyGreenlet_MAIN(this->main_greenlet_s));
            // If the main greenlet was current when the thread died (it
            // should be, right?) then we cleared its self pointer above
            // when we cleared the current greenlet's main greenlet pointer.
            assert(this->main_greenlet_s->super.main_greenlet_s == this->main_greenlet_s
                   || !this->main_greenlet_s->super.main_greenlet_s);
            // self reference, probably gone
            Py_CLEAR(this->main_greenlet_s->super.main_greenlet_s);
            Py_CLEAR(this->main_greenlet_s);
        }

        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(NULL);
            PyErr_Clear();
        }

    }

};

template<typename Destructor>
class ThreadStateCreator
{
private:
    ThreadState* _state;
    G_NO_COPIES_OF_CLS(ThreadStateCreator);
public:

    // Only one of these, auto created per thread
    ThreadStateCreator() :
        _state(0)
    {
    }

    ~ThreadStateCreator()
    {
        Destructor(this->_state);
    }

    inline ThreadState& state()
    {
        // The main greenlet will own this pointer when it is created,
        // which will be right after this. The plan is to give every
        // greenlet a pointer to the main greenlet for the thread it
        // runs in; if we are doing something cross-thread, we need to
        // access the pointer from the main greenlet. Deleting the
        // thread, and hence the thread-local storage, will delete the
        // state pointer in the main greenlet.
        if (!this->_state) {
            // XXX: Assumming allocation never fails
            this->_state = new ThreadState;
            // For non-standard threading, we need to store an object
            // in the Python thread state dictionary so that it can be
            // DECREF'd when the thread ends (ideally; the dict could
            // last longer) and clean this object up.
        }
        return *this->_state;
    }

    operator ThreadState&()
    {
        return this->state();
    }

    // shadow API to make this easier to use.
    inline PyGreenlet* borrow_target()
    {
        return this->state().borrow_target();
    };
    inline PyGreenlet* borrow_current()
    {
        return this->state().borrow_current();
    };
    inline PyGreenlet* borrow_origin()
    {
        return this->state().borrow_origin();
    };
    inline PyMainGreenlet* borrow_main_greenlet()
    {
        return this->state().borrow_main_greenlet();
    };

};

#if G_USE_STANDARD_THREADING == 1
// We can't use the PythonAllocator for this, because we push to it
// from the thread state destructor, which doesn't have the GIL,
// and Python's allocators can only be called with the GIL.
typedef std::vector<ThreadState*> cleanup_queue_t;
#else
class cleanup_queue_t {
public:
    inline ssize_t size() { return 0; };
    inline bool empty() { return true; };
    inline void pop_back()
    {
        throw std::out_of_range("empty queue.");
    };
    inline ThreadState* back()
    {
        throw std::out_of_range("empty queue.");
    };
    inline void push_back(ThreadState* g)
    {
        throw std::out_of_range("empty queue.");
    };
};
#endif
}; // namespace greenlet

#endif
