/*
 * Rut
 *
 * Rig Utilities
 *
 * Copyright (C) 2013,2014,2015  Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _RUT_CLOSURE_LIST_H_
#define _RUT_CLOSURE_LIST_H_

#include <clib.h>

/*
 * This implements a list of callbacks that can be used like signals
 * in GObject, but that don't have any marshalling overhead. The idea
 * is that any object that wants to provide a callback point will
 * provide a function to add a callback for that particular point. The
 * function can take a function pointer with the correct signature.
 * Internally the function will just call rut_closure_list_add. The
 * function should directly return a rut_closure_t pointer. The caller
 * can use this to disconnect the callback later without the object
 * having to provide a separate disconnect function.
 */

typedef void (*rut_closure_destroy_callback_t)(void *user_data);

typedef struct {
    c_list_t list_node;

#ifdef C_DEBUG
    c_list_t *owner;
    bool allocated;
#endif

    void *function;
    void *user_data;
    rut_closure_destroy_callback_t removed_cb;
} rut_closure_t;

/* XXX: In general these _init, _add_too, _remove and _remove_all apis
 * should be used over the previous _list_add, _disconnect and
 * _disconnect_all apis with closure structures that are embedded in
 * some other structure.
 *
 * These apis can't be intermixed; so you can't use _disconnect() a
 * closure that was passed to _init() or _remove() a closure created
 * via _list_add().
 *
 * XXX: The aim is to phase out and eventually remove all use of the
 * older closure apis.
 */
static inline void
rut_closure_init(rut_closure_t *closure,
                 void *function,
                 void *user_data)
{
    c_list_init(&closure->list_node);
    closure->function = function;
    closure->user_data = user_data;
    closure->removed_cb = NULL;

#ifdef C_DEBUG
    /* Help ensure we don't intermix the new and old closure
     * apis or accidentally try adding a closure to multiple
     * lists */
    closure->allocated = false;
    closure->owner = NULL;
#endif
}

static inline void
rut_closure_set_finalize(rut_closure_t *closure,
                         rut_closure_destroy_callback_t removed_cb)
{
    closure->removed_cb = removed_cb;
}

/* Note: it's ok to redundantly re-add a closure to a list without
 * manually checking for the redundancy, and it will be a nop */
void rut_closure_list_insert(c_list_t *list,
                             rut_closure_t *closure);
/* Note: it's ok to redundantly remove a closure not part of a
 * list without manually checking for the redundancy, and it will
 * be a nop */
void rut_closure_remove(rut_closure_t *closure);
void rut_closure_list_remove_all(c_list_t *list);


/* XXX: The aim is to phase out and eventually remove these older
 * closure apis...
 */
rut_closure_t *rut_closure_list_add(c_list_t *list,
                                    void *function,
                                    void *user_data,
                                    rut_closure_destroy_callback_t destroy_cb); //C_DEPRECATED("Use rut_closure_init + rut_closure_list_insert");
/**
 * rut_closure_disconnect:
 * @closure: A closure connected to a Rut closure list
 *
 * Removes the given closure from the callback list it is connected to
 * and destroys it. If the closure was created with a destroy function
 * then it will be invoked. */
void rut_closure_disconnect(rut_closure_t *closure); //C_DEPRECATED("Use rut_closure_remove");
void rut_closure_list_disconnect_all(c_list_t *list); //C_DEPRECATED("Use rut_closure_list_remove_all");

/**
 * rut_closure_list_invoke:
 * @list: A pointer to a c_list_t containing rut_closure_ts
 * @cb_type: The name of a typedef for the closure callback function signature
 * @...: The the arguments to pass to the callback
 *
 * A convenience macro to invoke a closure list.
 *
 * Note that the arguments will be evaluated multiple times so it is
 * not safe to pass expressions that have side-effects.
 *
 * Note also that this function ignores the return value from the
 * callbacks. If you want to handle the return value you should
 * manually iterate the list and invoke the callbacks yourself.
 */
#define rut_closure_list_invoke(list, cb_type, ...)                            \
    C_STMT_START                                                               \
    {                                                                          \
        rut_closure_t *_c, *_tmp;                                              \
                                                                               \
        c_list_for_each_safe(_c, _tmp, (list), list_node)                    \
        {                                                                      \
            cb_type _cb = _c->function;                                        \
            _cb(__VA_ARGS__, _c->user_data);                                   \
        }                                                                      \
    }                                                                          \
    C_STMT_END

#define rut_closure_list_invoke_no_args(list)                                  \
    C_STMT_START                                                               \
    {                                                                          \
        rut_closure_t *_c, *_tmp;                                              \
                                                                               \
        c_list_for_each_safe(_c, _tmp, (list), list_node)                    \
        {                                                                      \
            void (*_cb)(void *) = _c->function;                                \
            _cb(_c->user_data);                                                \
        }                                                                      \
    }                                                                          \
    C_STMT_END

#endif /* _RUT_CLOSURE_LIST_H_ */
