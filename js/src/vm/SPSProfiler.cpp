/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsnum.h"
#include "jsscript.h"

#include "vm/SPSProfiler.h"
#include "vm/StringBuffer.h"

using namespace js;

SPSProfiler::SPSProfiler(JSRuntime *rt)
  : rt(rt),
    stack_(NULL),
    size_(NULL),
    max_(0),
    slowAssertions(false),
    enabled_(false)
{
    JS_ASSERT(rt != NULL);
}

SPSProfiler::~SPSProfiler()
{
    if (strings.initialized()) {
        for (ProfileStringMap::Enum e(strings); !e.empty(); e.popFront())
            rt->array_delete(e.front().value);
    }
}

void
SPSProfiler::setProfilingStack(ProfileEntry *stack, uint32_t *size, uint32_t max)
{
    JS_ASSERT(!enabled());
    if (!strings.initialized())
        strings.init(max);
    stack_ = stack;
    size_  = size;
    max_   = max;
}

void
SPSProfiler::enable(bool enabled)
{
    JS_ASSERT(installed());
    enabled_ = enabled;
    /*
     * Ensure all future generated code will be instrumented, or that all
     * currently instrumented code is discarded
     */
    ReleaseAllJITCode(rt->defaultFreeOp());
}

/* Lookup the string for the function/script, creating one if necessary */
const char*
SPSProfiler::profileString(JSContext *cx, JSScript *script, JSFunction *maybeFun)
{
    JS_ASSERT(strings.initialized());
    ProfileStringMap::AddPtr s = strings.lookupForAdd(script);
    if (s)
        return s->value;
    const char *str = allocProfileString(cx, script, maybeFun);
    if (str == NULL)
        return NULL;
    if (!strings.add(s, script, str)) {
        rt->array_delete(str);
        return NULL;
    }
    return str;
}

void
SPSProfiler::onScriptFinalized(JSScript *script)
{
    /*
     * This function is called whenever a script is destroyed, regardless of
     * whether profiling has been turned on, so don't invoke a function on an
     * invalid hash set. Also, even if profiling was enabled but then turned
     * off, we still want to remove the string, so no check of enabled() is
     * done.
     */
    if (!strings.initialized())
        return;
    if (ProfileStringMap::Ptr entry = strings.lookup(script)) {
        const char *tofree = entry->value;
        strings.remove(entry);
        rt->array_delete(tofree);
    }
}

bool
SPSProfiler::enter(JSContext *cx, JSScript *script, JSFunction *maybeFun)
{
    const char *str = profileString(cx, script, maybeFun);
    if (str == NULL)
        return false;

    JS_ASSERT_IF(*size_ > 0 && *size_ - 1 < max_ && stack_[*size_ - 1].js(),
                 stack_[*size_ - 1].pc() != NULL);
    push(str, NULL, script, script->code);
    return true;
}

void
SPSProfiler::exit(JSContext *cx, JSScript *script, JSFunction *maybeFun)
{
    pop();

#ifdef DEBUG
    /* Sanity check to make sure push/pop balanced */
    if (*size_ < max_) {
        const char *str = profileString(cx, script, maybeFun);
        /* Can't fail lookup because we should already be in the set */
        JS_ASSERT(str != NULL);
        JS_ASSERT(stack_[*size_].js());
        JS_ASSERT(strcmp((const char*) stack_[*size_].label(), str) == 0);
        stack_[*size_].setLabel(NULL);
        stack_[*size_].setPC(NULL);
    }
#endif
}

void
SPSProfiler::push(const char *string, void *sp, JSScript *script, jsbytecode *pc)
{
    /* these operations cannot be re-ordered, so volatile-ize operations */
    volatile ProfileEntry *stack = stack_;
    volatile uint32_t *size = size_;
    uint32_t current = *size;

    JS_ASSERT(enabled());
    if (current < max_) {
        stack[current].setLabel(string);
        stack[current].setStackAddress(sp);
        stack[current].setScript(script);
        if (pc != NULL)
            stack_[current].setPC(pc);
    }
    *size = current + 1;
}

void
SPSProfiler::pop()
{
    JS_ASSERT(installed());
    (*size_)--;
    JS_ASSERT(*(int*)size_ >= 0);
}

/*
 * Serializes the script/function pair into a "descriptive string" which is
 * allowed to fail. This function cannot trigger a GC because it could finalize
 * some scripts, resize the hash table of profile strings, and invalidate the
 * AddPtr held while invoking allocProfileString.
 */
const char*
SPSProfiler::allocProfileString(JSContext *cx, JSScript *script, JSFunction *maybeFun)
{
    DebugOnly<uint64_t> gcBefore = cx->runtime->gcNumber;
    StringBuffer buf(cx);
    bool hasAtom = maybeFun != NULL && maybeFun->atom != NULL;
    if (hasAtom) {
        if (!buf.append(maybeFun->atom))
            return NULL;
        if (!buf.append(" ("))
            return NULL;
    }
    if (script->filename) {
        if (!buf.appendInflated(script->filename, strlen(script->filename)))
            return NULL;
    } else if (!buf.append("<unknown>")) {
        return NULL;
    }
    if (!buf.append(":"))
        return NULL;
    if (!NumberValueToStringBuffer(cx, NumberValue(script->lineno), buf))
        return NULL;
    if (hasAtom && !buf.append(")"))
        return NULL;

    size_t len = buf.length();
    char *cstr = rt->array_new<char>(len + 1);
    if (cstr == NULL)
        return NULL;

    const jschar *ptr = buf.begin();
    for (size_t i = 0; i < len; i++)
        cstr[i] = ptr[i];
    cstr[len] = 0;

    JS_ASSERT(gcBefore == cx->runtime->gcNumber);
    return cstr;
}

SPSEntryMarker::SPSEntryMarker(JSRuntime *rt JS_GUARD_OBJECT_NOTIFIER_PARAM_NO_INIT)
    : profiler(&rt->spsProfiler)
{
    JS_GUARD_OBJECT_NOTIFIER_INIT;
    if (!profiler->enabled()) {
        profiler = NULL;
        return;
    }
    profiler->push("js::RunScript", this, NULL, NULL);
}

SPSEntryMarker::~SPSEntryMarker()
{
    if (profiler != NULL)
        profiler->pop();
}

JS_FRIEND_API(jsbytecode*)
ProfileEntry::pc() volatile {
    return script()->code + idx;
}

JS_FRIEND_API(void)
ProfileEntry::setPC(jsbytecode *pc) volatile {
    idx = pc - script()->code;
}
