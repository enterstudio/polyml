/*
    Title:  An interpreter for a compact instruction set.
    Author:     Dave Matthews, Cambridge University Computer Laboratory

    Copyright (c) 2000-7
        Cambridge University Technical Services Limited
    Further development Copyright David C.J. Matthews 2015-16.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x) assert(x)
#else
#define ASSERT(x) 0
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#ifdef HAVE_MATH_H
#include <math.h>
#endif

#include "globals.h"
#include "int_opcodes.h"
#include "machine_dep.h"
#include "sys.h"
#include "profiling.h"
#include "arb.h"
#include "processes.h"
#include "run_time.h"
#include "mpoly.h"
#include "gc.h"
#include "basicio.h"
#include "timing.h"
#include "arb.h"
#include "reals.h"
#include "objsize.h"
#include "xwindows.h"
#include "foreign.h"
#include "process_env.h"
#include "network.h"
#include "basicio.h"
#include "sighandler.h"
#include "os_specific.h"
#include "diagnostics.h"
#include "polystring.h"
#include "save_vec.h"
#include "memmgr.h"
#include "poly_specific.h"
#include "scanaddrs.h"
#include "polyffi.h"
#include "rtsentry.h"

#define arg1    (pc[0] + pc[1]*256)
#define arg2    (pc[2] + pc[3]*256)
#define arg3    (pc[4] + pc[5]*256)
#define arg4    (pc[6] + pc[7]*256)

const PolyWord True = TAGGED(1);
const PolyWord False = TAGGED(0);
const PolyWord Zero = TAGGED(0);

#define CHECKED_REGS 2
#define UNCHECKED_REGS 0

#define EXTRA_STACK 0 // Don't need any extra - signals aren't handled on the Poly stack.

/* the amount of ML stack space to reserve for registers,
   C exception handling etc. The compiler requires us to
   reserve 2 stack-frames worth (2 * 20 words) plus whatever
   we require for the register save area. We actually reserve
   slightly more than this. SPF 3/3/97
*/
#define OVERFLOW_STACK_SIZE \
  (50 + \
   CHECKED_REGS + \
   UNCHECKED_REGS + \
   EXTRA_STACK)


// This duplicates some code in reals.cpp but is now updated.
#define DOUBLESIZE (sizeof(double)/sizeof(POLYUNSIGNED))

union realdb { double dble; POLYUNSIGNED puns[DOUBLESIZE]; };

class IntTaskData: public TaskData {
public:
    IntTaskData(): interrupt_requested(false), overflowPacket(0), dividePacket(0) {}

    virtual void GarbageCollect(ScanAddress *process);
    void ScanStackAddress(ScanAddress *process, PolyWord &val, StackSpace *stack);
    virtual Handle EnterPolyCode(); // Start running ML

    // Switch to Poly and return with the io function to call.
    int SwitchToPoly();
    virtual void SetException(poly_exn *exc);
    virtual void InterruptCode();

    // AddTimeProfileCount is used in time profiling.  We can't get accurate info so return false.
    virtual bool AddTimeProfileCount(SIGNALCONTEXT *context) { return false; }

    virtual void InitStackFrame(TaskData *newTask, Handle proc, Handle arg);

    // These aren't implemented in the interpreted version.
    virtual Handle EnterCallbackFunction(Handle func, Handle args) { ASSERT(0); return 0; }

    // Increment or decrement the first word of the object pointed to by the
    // mutex argument and return the new value.
    virtual Handle AtomicIncrement(Handle mutexp);
    // Set a mutex to one.
    virtual void AtomicReset(Handle mutexp);

    // Return the minimum space occupied by the stack.   Used when setting a limit.
    virtual POLYUNSIGNED currentStackSpace(void) const { return (this->stack->top - this->sp) + OVERFLOW_STACK_SIZE; }

    virtual void addProfileCount(POLYUNSIGNED words) { add_count(this, pc, words); }

    virtual void CopyStackFrame(StackObject *old_stack, POLYUNSIGNED old_length, StackObject *new_stack, POLYUNSIGNED new_length);

    bool interrupt_requested;

    // Allocate memory on the heap.  Returns with the address of the cell. Does not set the
    // length word or any of the data.
    PolyObject *allocateMemory(POLYUNSIGNED words)
    {
        words++; // Add the size of the length word.
        // N.B. The allocation area may be empty so that both of these are zero.
        if (this->allocPointer >= this->allocLimit + words)
        {
            this->allocPointer -= words;
            return (PolyObject *)(this->allocPointer+1);
        }
        // Insufficient space.
        // Find some space to allocate in. Returns a pointer to the newly allocated space.
        // N.B. This may return zero if the heap is exhausted and it has set this
        // up for an exception.  Generally it allocates by decrementing allocPointer
        // but if the required memory is large it may allocate in a separate area.
        PolyWord *space = processes->FindAllocationSpace(this, words, true);
        if (space == 0) return 0;
        return (PolyObject *)(space+1);
    }

    // Put a real result in a "box"
    PolyObject *boxDouble(double d)
    {
        PolyObject *mem = this->allocateMemory(DOUBLESIZE);
        if (mem == 0) return 0;
        mem->SetLengthWord(DOUBLESIZE, F_BYTE_OBJ);
        union realdb uniondb;
        uniondb.dble = d;
        // Copy the words.  Depending on the word length this may copy one or more words.
        for (unsigned i = 0; i < DOUBLESIZE; i++)
            mem->Set(i, PolyWord::FromUnsigned(uniondb.puns[i]));
        return mem;
    }

    // Extract a double value from a box.
    double unboxDouble(PolyWord p)
    {
        union realdb uniondb;
        for (unsigned i = 0; i < DOUBLESIZE; i++)
            uniondb.puns[i] = p.AsObjPtr()->Get(i).AsUnsigned();
        return uniondb.dble;
    }

    POLYCODEPTR     pc; /* Program counter. */
    PolyWord        *sp; /* Stack pointer. */
    PolyWord        *hr;
    PolyWord        exception_arg;
    unsigned        lastInstr; /* Last instruction. */
    PolyWord        *sl; /* Stack limit register. */

    PolyObject      *overflowPacket, *dividePacket;
};

// This lock is used to synchronise all atomic operations.
// It is not needed in the X86 version because that can use a global
// memory lock.
static PLock mutexLock;

// Special value for return address.
#define SPECIAL_PC_END_THREAD           TAGGED(1)

class Interpreter : public MachineDependent {
public:
    Interpreter() {}

    // Create a task data object.
    virtual TaskData *CreateTaskData(void) { return new IntTaskData(); }
    virtual Architectures MachineArchitecture(void) { return MA_Interpreted; }
};

void IntTaskData::InitStackFrame(TaskData *parentTask, Handle proc, Handle arg)
/* Initialise stack frame. */
{
    StackSpace *space = this->stack;
    StackObject *stack = (StackObject *)space->stack();
    PolyObject *closure = DEREFWORDHANDLE(proc);
    POLYUNSIGNED stack_size = space->spaceSize();
    this->pc = *(byte**)(closure);
    this->sp  = (PolyWord*)stack + stack_size-3; /* sp */
    this->exception_arg = TAGGED(0); /* Used for exception argument. */
    this->lastInstr = 256; /* No instruction. */
    this->sp  = (PolyWord*)stack + stack_size;

    /* Set up exception handler */
    /* No previous handler so point it at itself. */
    this->sp--;
    *(this->sp) = PolyWord::FromStackAddr(this->sp);
    *(--this->sp) = SPECIAL_PC_END_THREAD; /* Default return address. */
    this->hr = this->sp;

    /* If this function takes an argument store it on the stack. */
    if (arg != 0) *(--this->sp) = DEREFWORD(arg);

    *(--this->sp) = SPECIAL_PC_END_THREAD; /* Return address. */
    *(--this->sp) = closure; /* Closure address */

    // Make packets for exceptions.             
    Handle exn = make_exn(parentTask, EXC_overflow, parentTask->saveVec.push(TAGGED(0)));
    overflowPacket = exn->WordP();
    exn = make_exn(parentTask, EXC_divide, parentTask->saveVec.push(TAGGED(0)));
    dividePacket = exn->WordP();
}

extern "C" {
    typedef POLYUNSIGNED(*callFastRts0)();
    typedef POLYUNSIGNED(*callFastRts1)(PolyWord);
    typedef POLYUNSIGNED(*callFastRts2)(PolyWord, PolyWord);
    typedef POLYUNSIGNED(*callFastRts3)(PolyWord, PolyWord, PolyWord);
    typedef POLYUNSIGNED(*callFastRts4)(PolyWord, PolyWord, PolyWord, PolyWord);
    typedef POLYUNSIGNED(*callFastRts5)(PolyWord, PolyWord, PolyWord, PolyWord, PolyWord);
    typedef POLYUNSIGNED(*callFullRts0)(PolyObject *);
    typedef POLYUNSIGNED(*callFullRts1)(PolyObject *, PolyWord);
    typedef POLYUNSIGNED(*callFullRts2)(PolyObject *, PolyWord, PolyWord);
    typedef POLYUNSIGNED(*callFullRts3)(PolyObject *, PolyWord, PolyWord, PolyWord);
    typedef double (*callRTSFtoF) (double);
    typedef double (*callRTSGtoF) (PolyWord);
}

void IntTaskData::InterruptCode()
/* Stop the Poly code at a suitable place. */
/* We may get an asynchronous interrupt at any time. */
{
    IntTaskData *itd = (IntTaskData *)this;
    itd->interrupt_requested = true;
}


void IntTaskData::SetException(poly_exn *exc)
/* Set up the stack of a process to raise an exception. */
{
    this->lastInstr = INSTR_raise_ex;
    *(--this->sp) = (PolyWord)exc; /* push exception data */
}

int IntTaskData::SwitchToPoly()
/* (Re)-enter the Poly code from C. */
{
    // These are temporary values used where one instruction jumps to
    // common code.
    POLYUNSIGNED    tailCount;
    PolyWord        *tailPtr;
    POLYUNSIGNED    returnCount;
    POLYUNSIGNED    storeWords;

    sl = (PolyWord*)this->stack->stack()+OVERFLOW_STACK_SIZE;

    if (lastInstr != 256) goto RETRY; /* Re-execute instruction if necessary. */

    for(;;){ /* Each instruction */

        lastInstr = *pc++; /* Get next instruction. */

        RETRY:

        // Check for stack overflow and interrupts. These can be done less
        // frequently than every instruction.
        // Don't do this if we're raising an exception: we may be raising
        // it because we can't grow the stack.  Once we've processed the
        // exception and produced any exception trace the stack will have been
        // reset to the last handler.
        if (sp < sl && lastInstr != INSTR_raise_ex)
        {
            POLYUNSIGNED min_size = this->stack->top - this->sp + OVERFLOW_STACK_SIZE;
            CheckAndGrowStack(this, min_size);
            sl = (PolyWord*)this->stack->stack()+OVERFLOW_STACK_SIZE;
        }

        if (this->interrupt_requested) {
            this->interrupt_requested = false;
            return -1;
        }

        switch(lastInstr) {

        case INSTR_enter_int: pc++; /* Skip the argument. */ break;

        case INSTR_jump_false:
            {
                PolyWord u = *sp++; /* Pop argument */
                if (u == True) { pc += 1; break; }
                /* else - false - take the jump */
            }

        case INSTR_jump: pc += *pc + 1; break;

        case INSTR_push_handler: /* Save the old handler value. */
            *(--sp) = PolyWord::FromStackAddr(this->hr); /* Push old handler */
            break;

        case INSTR_set_handler_new: /* Set up a handler */
            *(--sp) = PolyWord::FromCodePtr(pc + *pc + 1); /* Address of handler */
            this->hr = sp;
            pc += 1;
            break;

        case INSTR_del_handler: /* Delete handler retaining the result. */
            {
                PolyWord u = *sp++;
                sp = this->hr;
                if (*sp == TAGGED(0)) sp++; // Legacy
                sp++; // Skip handler entry point
                // Restore old handler
                this->hr = (*sp).AsStackAddr();
                *sp = u; // Put back the result
                pc += *pc + 1; /* Skip the handler */
                break;
            }

        case INSTR_jump_i_false:
            if (*sp++ == True) { pc += 1; break; }
            /* else - false - take the jump */

        case INSTR_jump_i_u: /* Indirect jump */
            {
                // This is always a forward jump
                pc += *pc + 1;
                pc += arg1 + 2;
                break;
            }

        case INSTR_set_handler_new_i: /* Set up a handler */
            {
                byte *u = pc + *pc + 1;
                *(--sp) = /* Address of handler */
                    PolyWord::FromCodePtr(u + u[0] + u[1]*256 + 2);
                this->hr = sp;
                pc += 1;
                break;
            }

        case INSTR_del_handler_i: /* Delete handler retaining the result. */
            {
                PolyWord u = *sp++;
                PolyWord *t;
                sp = this->hr;
                PolyWord *endStack = this->stack->top;
                while((t = (*sp).AsStackAddr()) < sp || t > endStack) sp++;
                this->hr = t;
                *sp = u;
                pc += *pc + 1; /* Skip the handler */
                pc += arg1 + 2;
                break;
            }

        case INSTR_case:
            {
                POLYSIGNED u = UNTAGGED(*sp++); /* Get the value */
                if (u > arg1 || u < 0) pc += (arg1+2)*2; /* Out of range */
                else {
                    pc += 2;
                    pc += /* Index */pc[u*2]+pc[u*2 + 1]*256; }
                break;
            }

        case INSTR_tail_3_b:
           tailCount = 3;
           tailPtr = sp + tailCount;
           sp = tailPtr + *pc;
           goto TAIL_CALL;

        case INSTR_tail_3_2:
           tailCount = 3;
           tailPtr = sp + tailCount;
           sp = tailPtr + 2;
           goto TAIL_CALL;

        case INSTR_tail_3_3:
           tailCount = 3;
           tailPtr = sp + tailCount;
           sp = tailPtr + 3;
           goto TAIL_CALL;

        case INSTR_tail_4_b:
           tailCount = 4;
           tailPtr = sp + tailCount;
           sp = tailPtr + *pc;
           goto TAIL_CALL;

        case INSTR_tail_b_b:
           tailCount = *pc;
           tailPtr = sp + tailCount;
           sp = tailPtr + pc[1];
           goto TAIL_CALL;

        case INSTR_tail:
           /* Tail recursive call. */
           /* Move items up the stack. */
           /* There may be an overlap if the function we are calling
              has more args than this one. */
           tailCount = arg1;
           tailPtr = sp + tailCount;
           sp = tailPtr + arg2;
           TAIL_CALL: /* For general case. */
           if (tailCount < 2) Crash("Invalid argument\n");
           for (; tailCount > 0; tailCount--) *(--sp) = *(--tailPtr);
           pc = (*sp++).AsCodePtr(); /* Pop the original return address. */
           lastInstr = INSTR_call_closure; /* If we have to re-execute. */
           /* And drop through. */

        case INSTR_call_closure: /* Closure call. */
        {
            PolyWord *t = (*sp).AsStackAddr(); /* Closure */
            PolyWord u = *t;   /* Get code address. (1st word of closure) */
            sp--;
            *sp = sp[1];      /* Move closure up. */
            sp[1] = PolyWord::FromCodePtr(pc); /* Save return address. */
            pc = u.AsCodePtr();    /* Get entry point. */
            break;
        }

        case INSTR_return_w:
            returnCount = arg1; /* Get no. of args to remove. */

            RETURN: /* Common code for return. */
            {
                PolyWord result = *sp++; /* Result */
                sp++; /* Remove the link/closure */
                pc = (*sp++).AsCodePtr(); /* Return address */
                sp += returnCount; /* Add on number of args. */
                if (pc == (SPECIAL_PC_END_THREAD).AsCodePtr())
                    exitThread(this); // This thread is exiting.
                *(--sp) = result; /* Result */
            }
            break;

        case INSTR_return_b: returnCount = *pc; goto RETURN;
        case INSTR_return_0: returnCount = 0; goto RETURN;
        case INSTR_return_1: returnCount = 1; goto RETURN;
        case INSTR_return_2: returnCount = 2; goto RETURN;
        case INSTR_return_3: returnCount = 3; goto RETURN;

        case INSTR_pad: /* No-op */ break;

        case INSTR_raise_ex:
            {
            RAISE_EXCEPTION:
                PolyException *exn = (PolyException*)((*sp).AsObjPtr());
                this->exception_arg = exn; /* Get exception data */
                sp = this->hr;
                if (*sp == SPECIAL_PC_END_THREAD)
                    exitThread(this);  // Default handler for thread.
                this->pc = (*sp++).AsCodePtr();
                this->hr = (*sp++).AsStackAddr();
                break;
            }

        case INSTR_get_store_w:
        // Get_store is now only used for mutually recursive closures.  It allocates mutable store
        // initialised to zero.
        case INSTR_legacy_container: /* Create a container. */
                /* This is supposed to be on the stack but that causes problems in gencde
                   so we create a mutable segment on the heap. */
        {
            storeWords = arg1;
            pc += 2;
            GET_STORE:
            PolyObject *p = this->allocateMemory(storeWords);
            if (p == 0) goto RAISE_EXCEPTION;
            p->SetLengthWord(storeWords, F_MUTABLE_BIT);
            for(; storeWords > 0; ) p->Set(--storeWords, TAGGED(0)); /* Must initialise store! */
            *(--sp) = p;
            break;
        }

        case INSTR_get_store_2: storeWords = 2; goto GET_STORE;
        case INSTR_get_store_3: storeWords = 3; goto GET_STORE;
        case INSTR_get_store_4: storeWords = 4; goto GET_STORE;
        case INSTR_get_store_b: storeWords = *pc; pc++; goto GET_STORE;

        case INSTR_tuple_w:
        {
            storeWords = arg1; pc += 2;
        TUPLE: /* Common code for tupling. */
            PolyObject *p = this->allocateMemory(storeWords);
            if (p == 0) goto RAISE_EXCEPTION; // Exception
            p->SetLengthWord(storeWords, 0);
            for(; storeWords > 0; ) p->Set(--storeWords, *sp++);
            *(--sp) = p;
            break;
        }

        case INSTR_tuple_2: storeWords = 2; goto TUPLE;
        case INSTR_tuple_3: storeWords = 3; goto TUPLE;
        case INSTR_tuple_4: storeWords = 4; goto TUPLE;
        case INSTR_tuple_b: storeWords = *pc; pc++; goto TUPLE;

        case INSTR_non_local:
            {
                PolyWord *t = sp+arg1;
                POLYSIGNED uu;
                for(uu = 1; uu <= arg2; uu++) t = (t[-1]).AsStackAddr();
                uu = arg3; /* Can be negative. */
                if (uu > 32767) uu -= 65536;
                *(--sp) = t[uu];
                pc += 6;
                break;
            }

        case INSTR_local_w:
            {
                PolyWord u = sp[arg1];
                *(--sp) = u;
                pc += 2;
                break;
            }

        case INSTR_indirect_w:
            *sp = (*sp).AsObjPtr()->Get(arg1); pc += 2; break;

        case INSTR_move_to_vec_w:
            {
                PolyWord u = *sp++;
                (*sp).AsObjPtr()->Set(arg1, u);
                pc += 2;
                break;
            }

        case INSTR_set_stack_val_w:
            {
                PolyWord u = *sp++;
                sp[arg1-1] = u;
                pc += 2;
                break;
            }

        case INSTR_reset_w: sp += arg1; pc += 2; break;

        case INSTR_reset_r_w:
            {
                PolyWord u = *sp;
                sp += arg1;
                *sp = u;
                pc += 2;
                break;
            }

        case INSTR_const_addr:
            *(--sp) = *(PolyWord*)(pc + arg1 + 2); pc += 2; break;

        case INSTR_const_addr_Xb:
            *(--sp) = (PolyWord::FromCodePtr(pc + (pc[0]+4)*sizeof(PolyWord) + pc[1] + pc[2]*256 + 3)).AsObjPtr()->Get(0);
            pc += 3;
            break;

        case INSTR_const_addr_Xw:
            *(--sp) = (PolyWord::FromCodePtr(pc + (arg1+4)*sizeof(PolyWord)+arg2 + 4)).AsObjPtr()->Get(0);
            pc += 4;
            break;

        case INSTR_const_int_w: *(--sp) = TAGGED(arg1); pc += 2; break;

        case INSTR_jump_back: pc -= *pc + 1; break;

        case INSTR_jump_back16:
            pc -= arg1 + 1; break;

        case INSTR_lock:
            {
                PolyObject *obj = (*sp).AsObjPtr();
                obj->SetLengthWord(obj->LengthWord() & ~_OBJ_MUTABLE_BIT);
                break;
            }

        case INSTR_ldexc: *(--sp) = this->exception_arg; break;

        case INSTR_local_b: { PolyWord u = sp[*pc]; *(--sp) = u; pc += 1; break; }

        case INSTR_indirect_b:
            *sp = (*sp).AsObjPtr()->Get(*pc); pc += 1; break;

        case INSTR_move_to_vec_b:
            { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(*pc, u); pc += 1; break; }

        case INSTR_set_stack_val_b:
            { PolyWord u = *sp++; sp[*pc-1] = u; pc += 1; break; }

        case INSTR_reset_b: sp += *pc; pc += 1; break;

        case INSTR_reset_r_b:
            { PolyWord u = *sp; sp += *pc; *sp = u; pc += 1; break; }

        case INSTR_const_int_b: *(--sp) = TAGGED(*pc); pc += 1; break;

        case INSTR_local_0: { PolyWord u = sp[0]; *(--sp) = u; break; }
        case INSTR_local_1: { PolyWord u = sp[1]; *(--sp) = u; break; }
        case INSTR_local_2: { PolyWord u = sp[2]; *(--sp) = u; break; }
        case INSTR_local_3: { PolyWord u = sp[3]; *(--sp) = u; break; }
        case INSTR_local_4: { PolyWord u = sp[4]; *(--sp) = u; break; }
        case INSTR_local_5: { PolyWord u = sp[5]; *(--sp) = u; break; }
        case INSTR_local_6: { PolyWord u = sp[6]; *(--sp) = u; break; }
        case INSTR_local_7: { PolyWord u = sp[7]; *(--sp) = u; break; }
        case INSTR_local_8: { PolyWord u = sp[8]; *(--sp) = u; break; }
        case INSTR_local_9: { PolyWord u = sp[9]; *(--sp) = u; break; }
        case INSTR_local_10: { PolyWord u = sp[10]; *(--sp) = u; break; }
        case INSTR_local_11: { PolyWord u = sp[11]; *(--sp) = u; break; }

        case INSTR_indirect_0:
            *sp = (*sp).AsObjPtr()->Get(0); break;

        case INSTR_indirect_1:
            *sp = (*sp).AsObjPtr()->Get(1); break;

        case INSTR_indirect_2:
            *sp = (*sp).AsObjPtr()->Get(2); break;

        case INSTR_indirect_3:
            *sp = (*sp).AsObjPtr()->Get(3); break;

        case INSTR_indirect_4:
            *sp = (*sp).AsObjPtr()->Get(4); break;

        case INSTR_indirect_5:
            *sp = (*sp).AsObjPtr()->Get(5); break;

        case INSTR_const_0: *(--sp) = Zero; break;
        case INSTR_const_1: *(--sp) = TAGGED(1); break;
        case INSTR_const_2: *(--sp) = TAGGED(2); break;
        case INSTR_const_3: *(--sp) = TAGGED(3); break;
        case INSTR_const_4: *(--sp) = TAGGED(4); break;
        case INSTR_const_10: *(--sp) = TAGGED(10); break;

        case INSTR_move_to_vec_0:  { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(0, u); break; }
        case INSTR_move_to_vec_1: { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(1, u); break; }
        case INSTR_move_to_vec_2: { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(2, u); break; }
        case INSTR_move_to_vec_3: { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(3, u); break; }
        case INSTR_move_to_vec_4: { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(4, u); break; }
        case INSTR_move_to_vec_5: { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(5, u); break; }
        case INSTR_move_to_vec_6: { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(6, u); break; }
        case INSTR_move_to_vec_7: { PolyWord u = *sp++; (*sp).AsObjPtr()->Set(7, u); break; }

        case INSTR_reset_r_1: { PolyWord u = *sp; sp += 1; *sp = u; break; }
        case INSTR_reset_r_2: { PolyWord u = *sp; sp += 2; *sp = u; break; }
        case INSTR_reset_r_3: { PolyWord u = *sp; sp += 3; *sp = u; break; }

        case INSTR_reset_1: sp += 1; break;
        case INSTR_reset_2: sp += 2; break;


        case INSTR_non_local_l_1:
            {
                POLYSIGNED uu = *pc;
                PolyWord u = (sp[uu >> 4]).AsStackAddr()[(uu & 0xf) - 6];
                *(--sp) = u;
                pc += 1;
                break;
            }

        case INSTR_non_local_l_2:
            {
                POLYSIGNED uu = *pc;
                PolyWord *t = sp[uu >> 4].AsStackAddr() -1;
                *(--sp) = (*t).AsStackAddr()[(uu & 0xf) - 6];
                pc += 1;
                break;
            }

        case INSTR_non_local_l_3:
            {
                POLYSIGNED uu = *pc;
                PolyWord *t = sp[uu >> 4].AsStackAddr() -1;
                t = (*t).AsStackAddr() - 1;
                *(--sp) = (*t).AsStackAddr()[(uu & 0xf) - 6];
                pc += 1; break;
            }

        case INSTR_set_container: /* Copy a tuple into a container. */
            {
                PolyWord u = *sp++; /* Pop the source tuple address. */
                for (POLYSIGNED uu = arg1; uu > 0; )
                {
                    uu--;
                    (*sp).AsObjPtr()->Set(uu, u.AsObjPtr()->Get(uu)); /* Copy the items. */
                }
                sp++;
                pc += 2;
                break;
            }

        case INSTR_stack_container:
        {
            POLYUNSIGNED words = arg1; pc += 2;
            while (words-- > 0) *(--sp) = Zero;
            sp--;
            *sp = PolyWord::FromStackAddr(sp+1);
            break;
        }

        case INSTR_tuple_container: /* Create a tuple from a container. */
            {
                storeWords = arg1;
                PolyObject *t = this->allocateMemory(storeWords);
                if (t == 0) goto RAISE_EXCEPTION;
                t->SetLengthWord(storeWords, 0);
                for(; storeWords > 0; )
                {
                    storeWords--;
                    t->Set(storeWords, (*sp).AsObjPtr()->Get(storeWords));
                }
                *sp = t;
                pc += 2;
                break;
            }

        case INSTR_callFastRTS0:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                callFastRts0 doCall = (callFastRts0)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall();
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS1:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg1 = *sp++;
                callFastRts1 doCall = (callFastRts1)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(rtsArg1);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS2:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg2 = *sp++; // Pop off the args, last arg first.
                PolyWord rtsArg1 = *sp++;
                callFastRts2 doCall = (callFastRts2)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS3:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg3 = *sp++; // Pop off the args, last arg first.
                PolyWord rtsArg2 = *sp++;
                PolyWord rtsArg1 = *sp++;
                callFastRts3 doCall = (callFastRts3)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2, rtsArg3);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS4:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg4 = *sp++; // Pop off the args, last arg first.
                PolyWord rtsArg3 = *sp++;
                PolyWord rtsArg2 = *sp++;
                PolyWord rtsArg1 = *sp++;
                callFastRts4 doCall = (callFastRts4)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2, rtsArg3, rtsArg4);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastRTS5:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg5 = *sp++; // Pop off the args, last arg first.
                PolyWord rtsArg4 = *sp++;
                PolyWord rtsArg3 = *sp++;
                PolyWord rtsArg2 = *sp++;
                PolyWord rtsArg1 = *sp++;
                callFastRts5 doCall = (callFastRts5)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(rtsArg1, rtsArg2, rtsArg3, rtsArg4, rtsArg5);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS0:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                callFullRts0 doCall = (callFullRts0)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(this->threadObject);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS1:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg1 = *sp++;
                callFullRts1 doCall = (callFullRts1)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(this->threadObject, rtsArg1);
                sp = this->sp; // May have changed if there was an exception
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS2:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg2 = *sp++; // Pop off the args, last arg first.
                PolyWord rtsArg1 = *sp++;
                callFullRts2 doCall = (callFullRts2)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(this->threadObject, rtsArg1, rtsArg2);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFullRTS3:
            {
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg3 = *sp++; // Pop off the args, last arg first.
                PolyWord rtsArg2 = *sp++;
                PolyWord rtsArg1 = *sp++;
                callFullRts3 doCall = (callFullRts3)rtsCall.AsCodePtr();
                POLYUNSIGNED result = doCall(this->threadObject, rtsArg1, rtsArg2, rtsArg3);
                // If this raised an exception 
                if (this->lastInstr == INSTR_raise_ex) goto RAISE_EXCEPTION;
                *(--sp) = PolyWord::FromUnsigned(result);
                break;
            }

        case INSTR_callFastFtoF:
            {
                // Floating point call.  The call itself does not allocate but we
                // need to put the result into a "box".
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg1 = *sp++;
                callRTSFtoF doCall = (callRTSFtoF)rtsCall.AsCodePtr();
                double argument = unboxDouble(rtsArg1);
                // Allocate memory for the result.
                double result = doCall(argument);
                PolyObject *t = boxDouble(result);
                if (t == 0) goto RAISE_EXCEPTION;
                *(--sp) = t;
                break;
            }

        case INSTR_callFastGtoF:
            {
                // Call that takes a POLYUNSIGNED argument and returns a double.
                PolyWord rtsCall = (*sp++).AsObjPtr()->Get(0); // Value holds address.
                PolyWord rtsArg1 = *sp++;
                callRTSGtoF doCall = (callRTSGtoF)rtsCall.AsCodePtr();
                // Allocate memory for the result.
                double result = doCall(rtsArg1);
                PolyObject *t = boxDouble(result);
                if (t == 0) goto RAISE_EXCEPTION;
                *(--sp) = t;
                break;
            }

        case INSTR_notBoolean:
            *sp = (*sp == True) ? False : True; break;

        case INSTR_isTagged:
            *sp = IS_INT(*sp) ? True : False; break;

        case INSTR_cellLength:
            /* Return the length word. */
            *sp = TAGGED((*sp).AsObjPtr()->Length());
            break;

        case INSTR_cellFlags:
        {
            PolyObject *p = (*sp).AsObjPtr();
            POLYUNSIGNED f = (p->LengthWord()) >> OBJ_PRIVATE_FLAGS_SHIFT;
            *sp = TAGGED(f);
            break;
        }

        case INSTR_clearMutable:
        {
            PolyObject *obj = (*sp).AsObjPtr();
            POLYUNSIGNED lengthW = obj->LengthWord();
            /* Clear the mutable bit. */
            obj->SetLengthWord(lengthW & ~_OBJ_MUTABLE_BIT);
            *sp = Zero;
            break;
        }

        case INSTR_stringLength: // Now replaced by loadUntagged
            *sp = TAGGED(((PolyStringObject*)(*sp).AsObjPtr())->length);
            break;

        case INSTR_atomicIncr:
        {
            PLocker l(&mutexLock);
            PolyObject *p = (*sp).AsObjPtr();
            PolyWord newValue = TAGGED(UNTAGGED(p->Get(0))+1);
            p->Set(0, newValue);
            *sp = newValue;
            break;
        }

        case INSTR_atomicDecr:
        {
            PLocker l(&mutexLock);
            PolyObject *p = (*sp).AsObjPtr();
            PolyWord newValue = TAGGED(UNTAGGED(p->Get(0))-1);
            p->Set(0, newValue);
            *sp = newValue;
            break;
        }

        case INSTR_atomicReset:
        {
            // This is needed in the interpreted version otherwise there
            // is a chance that we could set the value to zero while another
            // thread is between getting the old value and setting it to the new value.
            PLocker l(&mutexLock);
            PolyObject *p = (*sp).AsObjPtr();
            p->Set(0, TAGGED(1)); // Set this to released.
            *sp = TAGGED(0); // Push the unit result
            break;
        }

        case INSTR_longWToTagged:
        {
            // Extract the first word and return it as a tagged value.  This loses the top-bit
            POLYUNSIGNED wx = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            *sp = TAGGED(wx);
            break;
        }

        case INSTR_signedToLongW:
        {
            // Shift the tagged value to remove the tag and put it into the first word.
            // The original sign bit is copied in the shift.
            POLYSIGNED wx = (*sp).UnTagged();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromSigned(wx));
            *sp = t;
            break;
        }

        case INSTR_unsignedToLongW:
        {
            // As with the above except the value is treated as an unsigned
            // value and the top bit is zero.
            POLYUNSIGNED wx = (*sp).UnTaggedUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wx));
            *sp = t;
            break;
        }

        case INSTR_realAbs:
        {
            PolyObject *t = this->boxDouble(fabs(unboxDouble(*sp)));
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_realNeg:
        {
            PolyObject *t = this->boxDouble(-(unboxDouble(*sp)));
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_floatFixedInt:
        {
            POLYSIGNED u = UNTAGGED(*sp);
            PolyObject *t = this->boxDouble((double)u);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_equalWord:
        {
            PolyWord u = *sp++;
            *sp = u == *sp ? True : False;
            break;
        }

        case INSTR_lessSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsSigned() < u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_lessUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsUnsigned() < u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_lessEqSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsSigned() <= u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_lessEqUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsUnsigned() <= u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_greaterSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsSigned() > u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_greaterUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsUnsigned() > u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_greaterEqSigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsSigned() >= u.AsSigned()) ? True : False;
            break;
        }

        case INSTR_greaterEqUnsigned:
        {
            PolyWord u = *sp++;
            *sp = ((*sp).AsUnsigned() >= u.AsUnsigned()) ? True : False;
            break;
        }

        case INSTR_fixedAdd:
        {
            PolyWord x = *sp++;
            PolyWord y = *sp;
            POLYSIGNED t = UNTAGGED(x) + UNTAGGED(y);
            if (t <= MAXTAGGED && t >= -MAXTAGGED-1)
                *sp = TAGGED(t);
            else
            {
                *(--sp) = overflowPacket;
                goto RAISE_EXCEPTION;
            }
            break;
        }

        case INSTR_fixedSub:
        {
            PolyWord x = *sp++;
            PolyWord y = *sp;
            POLYSIGNED t = UNTAGGED(y) - UNTAGGED(x);
            if (t <= MAXTAGGED && t >= -MAXTAGGED-1)
                *sp = TAGGED(t);
            else
            {
                *(--sp) = overflowPacket;
                goto RAISE_EXCEPTION;
            }
            break;
        }

        case INSTR_fixedMult:
        {
            // We need to detect overflow.  There doesn't seem to be any convenient way to do
            // this so we use the arbitrary precision package and check whether the result is short.
            // Clang and GCC 5.0 have __builtin_mul_overflow which will do this but GCC 5.0 is not
            // currently (July 2016) in Debian stable.
            Handle reset = this->saveVec.mark();
            Handle pushedArg1 = this->saveVec.push(*sp++);
            Handle pushedArg2 = this->saveVec.push(*sp);
            Handle result = mult_longc(this, pushedArg2, pushedArg1);
            PolyWord res = result->Word();
            this->saveVec.reset(reset);
            if (! res.IsTagged()) 
            {
                *(--sp) = overflowPacket;
                goto RAISE_EXCEPTION;
            }
            *sp = res;
            break;
        }

        case INSTR_fixedQuot:
        {
            // Zero and overflow are checked for in ML.
            POLYSIGNED u = UNTAGGED(*sp++);
            PolyWord y = *sp;
            *sp = TAGGED(UNTAGGED(y) / u);
            break;
        }

        case INSTR_fixedRem:
        {
            // Zero and overflow are checked for in ML.
            POLYSIGNED u = UNTAGGED(*sp++);
            PolyWord y = *sp;
            *sp = TAGGED(UNTAGGED(y) % u);
            break;
        }

        case INSTR_wordAdd:
        {
            PolyWord u = *sp++;
            // Because we're not concerned with overflow we can just add the values and subtract the tag.
            *sp = PolyWord::FromUnsigned((*sp).AsUnsigned() + u.AsUnsigned() - TAGGED(0).AsUnsigned());
            break;
        }

        case INSTR_wordSub:
        {
            PolyWord u = *sp++;
            *sp = PolyWord::FromUnsigned((*sp).AsUnsigned() - u.AsUnsigned() + TAGGED(0).AsUnsigned());
            break;
        }

        case INSTR_wordMult:
        {
            PolyWord u = *sp++;
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) * UNTAGGED_UNSIGNED(u));
            break;
        }

        case INSTR_wordDiv:
        {
            POLYUNSIGNED u = UNTAGGED_UNSIGNED(*sp++);
            // Detection of zero is done in ML
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) / u); break;
        }

        case INSTR_wordMod:
        {
            POLYUNSIGNED u = UNTAGGED_UNSIGNED(*sp++);
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) % u);
            break;
        }

        case INSTR_setStringLength: // Now replaced by storeUntagged
        {
            /* Store the length word of a string. */
            POLYUNSIGNED len = UNTAGGED(*sp++);
            ((PolyStringObject*)(*sp).AsObjPtr())->length = len;
            *sp = Zero;
            break; 
        }

        case INSTR_wordAnd:
        {
            PolyWord u = *sp++;
            // Since both of these should be tagged the tag bit will be preserved.
            *sp = PolyWord::FromUnsigned((*sp).AsUnsigned() & u.AsUnsigned());
            break;
        }

        case INSTR_wordOr:
        {
            PolyWord u = *sp++;
            // Since both of these should be tagged the tag bit will be preserved.
            *sp = PolyWord::FromUnsigned((*sp).AsUnsigned() | u.AsUnsigned());
            break;
        }

        case INSTR_wordXor:
        {
            PolyWord u = *sp++;
            // This will remove the tag bit so it has to be reinstated.
            *sp = PolyWord::FromUnsigned(((*sp).AsUnsigned() ^ u.AsUnsigned()) | TAGGED(0).AsUnsigned());
            break;
        }

        case INSTR_wordShiftLeft:
        {
            // ML requires shifts greater than a word to return zero. 
            // That's dealt with at the higher level.
            PolyWord u = *sp++;
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) << UNTAGGED_UNSIGNED(u));
            break;
        }

        case INSTR_wordShiftRLog:
        {
            PolyWord u = *sp++;
            *sp = TAGGED(UNTAGGED_UNSIGNED(*sp) >> UNTAGGED_UNSIGNED(u));
            break;
        }

        case INSTR_wordShiftRArith:
        {
            PolyWord u = *sp++;
            // Strictly speaking, C does not require that this uses
            // arithmetic shifting so we really ought to set the
            // high-order bits explicitly.
            *sp = TAGGED(UNTAGGED(*sp) >> UNTAGGED(u));
            break;
        }

        case INSTR_allocByteMem:
        {
            // Allocate byte segment.  This does not need to be initialised.
            POLYUNSIGNED flags = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp);
            PolyObject *t = this->allocateMemory(length);
            if (t == 0) goto RAISE_EXCEPTION; // Exception
            t->SetLengthWord(length, (byte)flags);
            *sp = t;
            break;
        }

        case INSTR_lgWordEqual:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            *sp = wx == wy ? True : False;
            break;
        }

        case INSTR_lgWordNotequal:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            *sp = wx != wy ? True : False;
            break;
        }

        case INSTR_lgWordLess:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            *sp = (wy < wx) ? True : False;
            break;
        }

        case INSTR_lgWordLessEq:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            *sp = (wy <= wx) ? True : False;
            break;
        }

        case INSTR_lgWordGreater:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            *sp = (wy > wx) ? True : False;
            break;
        }

        case INSTR_lgWordGreaterEq:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            *sp = (wy >= wx) ? True : False;
            break;
        }

        case INSTR_lgWordAdd:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy+wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordSub:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy-wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordMult:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy*wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordDiv:
         {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy/wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordMod:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy%wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordAnd:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy&wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordOr:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy|wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordXor:
        {
            POLYUNSIGNED wx = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy^wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordShiftLeft:
        {
            // The shift amount is a tagged word not a boxed large word
            POLYUNSIGNED wx = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy << wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordShiftRLog:
        {
            // The shift amount is a tagged word not a boxed large word
            POLYUNSIGNED wx = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED wy = (*sp).AsObjPtr()->Get(0).AsUnsigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(wy >> wx));
            *sp = t;
            break;
        }

        case INSTR_lgWordShiftRArith:
        {
            // The shift amount is a tagged word not a boxed large word
            POLYUNSIGNED wx = UNTAGGED_UNSIGNED(*sp++);
            POLYSIGNED wy = (*sp).AsObjPtr()->Get(0).AsSigned();
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromSigned(wy >> wx));
            *sp = t;
            break;
        }

        case INSTR_realEqual:
        {
            double u = unboxDouble(*sp++);
            *sp = u == unboxDouble(*sp) ? True: False;
            break;
        }

        case INSTR_realLess:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) < u ? True: False;
            break;
        }

        case INSTR_realLessEq:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) <= u ? True: False;
            break;
        }

        case INSTR_realGreater:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) > u ? True: False;
            break;
        }

        case INSTR_realGreaterEq:
        {
            double u = unboxDouble(*sp++);
            *sp =  unboxDouble(*sp) >= u ? True: False;
            break;
        }

        case INSTR_realAdd:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v+u);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_realSub:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v-u);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_realMult:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v*u);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_realDiv:
        {
            double u = unboxDouble(*sp++);
            double v = unboxDouble(*sp);
            PolyObject *t = this->boxDouble(v/u);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_getThreadId:
            *(--sp) = this->threadObject;
            break;

        case INSTR_allocWordMemory:
        {
            // Allocate word segment.  This must be initialised.
            // We mustn't pop the initialiser until after any potential GC.
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(sp[2]);
            PolyObject *t = this->allocateMemory(length);
            if (t == 0) goto RAISE_EXCEPTION;
            PolyWord initialiser = *sp++;
            POLYUNSIGNED flags = UNTAGGED_UNSIGNED(*sp++);
            t->SetLengthWord(length, (byte)flags);
            *sp = t;
            // Have to initialise the data.
            for (; length > 0; ) t->Set(--length, initialiser);
            break;
        }

        case INSTR_alloc_ref:
        {
            // Allocate a single word mutable cell.  This is more common than allocWordMemory on its own.
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            PolyWord initialiser = *sp;
            t->SetLengthWord(1, F_MUTABLE_BIT);
            t->Set(0, initialiser);
            *sp = t;
            break;
        }

        case INSTR_loadMLWord:
        {
            // The values on the stack are base, index and offset.
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).AsCodePtr() + offset);
            *sp = p->Get(index);
            break;
        }

        case INSTR_loadMLByte:
        {
            // The values on the stack are base and index.
            POLYUNSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsCodePtr();
            *sp = TAGGED(p[index]); // Have to tag the result
            break;
        }

        case INSTR_loadC8:
        {
            // This is similar to loadMLByte except that the base address is a boxed large-word.
            // Also the index is SIGNED.
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr();
            *sp = TAGGED(p[index]); // Have to tag the result
            break;
        }

        case INSTR_loadC16:
        {
            // This and the other loads are similar to loadMLWord with separate
            // index and offset values.
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            POLYUNSIGNED r = ((uint16_t*)p)[index];
            *sp = TAGGED(r);
            break;
        }

        case INSTR_loadC32:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            POLYUNSIGNED r = ((uint32_t*)p)[index];
#if (SIZEOF_VOIDP == 8)
            // This is tagged in 64-bit mode
            *sp = TAGGED(r);
#else
            // But boxed in 32-bit mode.
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(r));
            *sp = t;
#endif
            break;
        }
#if (SIZEOF_VOIDP == 8)
        case INSTR_loadC64:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            POLYUNSIGNED r = ((uint64_t*)p)[index];
            // This must be boxed.
            PolyObject *t = this->allocateMemory(1);
            if (t == 0) goto RAISE_EXCEPTION;
            t->SetLengthWord(1, F_BYTE_OBJ);
            t->Set(0, PolyWord::FromUnsigned(r));
            *sp = t;
            break;
        }
#endif

        case INSTR_loadCFloat:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            double r = ((float*)p)[index];
            // This must be boxed.
            PolyObject *t = this->boxDouble(r);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_loadCDouble:
        {
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            double r = ((double*)p)[index];
            // This must be boxed.
            PolyObject *t = this->boxDouble(r);
            if (t == 0) goto RAISE_EXCEPTION;
            *sp = t;
            break;
        }

        case INSTR_loadUntagged:
        {
            // The values on the stack are base, index and offset.
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).AsCodePtr() + offset);
            *sp = TAGGED(p->Get(index).AsUnsigned());
            break;
        }

        case INSTR_storeMLWord: 
        {
            PolyWord toStore = *sp++;
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).AsCodePtr() + offset);
            p->Set(index, toStore);
            *sp = Zero;
            break;
        }

        case INSTR_storeMLByte: 
        {
            POLYUNSIGNED toStore = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsCodePtr();
            p[index] = (byte)toStore;
            *sp = Zero;
            break; 
        }

        case INSTR_storeC8: 
        {
            // Similar to storeMLByte except that the base address is a boxed large-word.
            POLYUNSIGNED toStore = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr();
            p[index] = (byte)toStore;
            *sp = Zero;
            break; 
        }

        case INSTR_storeC16:
        {
            uint16_t toStore = (uint16_t)UNTAGGED(*sp++);
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            ((uint16_t*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

        case INSTR_storeC32:
        {
#if (SIZEOF_VOIDP == 8)
            // This is a tagged value in 64-bit mode.
            uint32_t toStore = (uint32_t)UNTAGGED(*sp++);
#else
            // but a boxed value in 32-bit mode.
            uint32_t toStore = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
#endif
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            ((uint32_t*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

#if (SIZEOF_VOIDP == 8)
        case INSTR_storeC64:
        {
            // This is a boxed value.
            uint64_t toStore = (*sp++).AsObjPtr()->Get(0).AsUnsigned();
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            ((uint64_t*)p)[index] = toStore;
            *sp = Zero;
            break;
        }
#endif

        case INSTR_storeCFloat:
        {
            // This is a boxed value.
            float toStore = (float)unboxDouble(*sp++);
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            ((float*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

        case INSTR_storeCDouble:
        {
            // This is a boxed value.
            double toStore = unboxDouble(*sp++);
            POLYSIGNED offset = UNTAGGED(*sp++);
            POLYSIGNED index = UNTAGGED(*sp++);
            POLYCODEPTR p = (*sp).AsObjPtr()->Get(0).AsCodePtr() + offset;
            ((double*)p)[index] = toStore;
            *sp = Zero;
            break;
        }

        case INSTR_storeUntagged: 
        {
            PolyWord toStore = PolyWord::FromUnsigned(UNTAGGED_UNSIGNED(*sp++));
            POLYUNSIGNED offset = UNTAGGED(*sp++);
            POLYUNSIGNED index = UNTAGGED(*sp++);
            PolyObject *p = (PolyObject*)((*sp).AsCodePtr() + offset);
            p->Set(index, toStore);
            *sp = Zero;
            break;
        }

        case INSTR_blockMoveWord:
        {
            // The offsets are byte counts but the the indexes are in words.
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED destOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED destIndex = UNTAGGED_UNSIGNED(*sp++);
            PolyObject *dest = (PolyObject*)((*sp++).AsCodePtr() + destOffset);
            POLYUNSIGNED srcOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED srcIndex = UNTAGGED_UNSIGNED(*sp++);
            PolyObject *src = (PolyObject*)((*sp).AsCodePtr() + srcOffset);
            for (POLYUNSIGNED u = 0; u < length; u++) dest->Set(destIndex+u, src->Get(srcIndex+u));
            *sp = Zero;
            break;
        }

        case INSTR_blockMoveByte:
        {
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED destOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR dest = (*sp++).AsCodePtr();
            POLYUNSIGNED srcOffset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR src = (*sp).AsCodePtr();
            memcpy(dest+destOffset, src+srcOffset, length);
            *sp = Zero;
            break;
        }

        case INSTR_blockEqualByte:
        {
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED arg2Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg2Ptr = (*sp++).AsCodePtr();
            POLYUNSIGNED arg1Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg1Ptr = (*sp).AsCodePtr();
            *sp = memcmp(arg1Ptr+arg1Offset, arg2Ptr+arg2Offset, length) == 0 ? True : False;
            break;
        }

        case INSTR_blockCompareByte:
        {
            POLYUNSIGNED length = UNTAGGED_UNSIGNED(*sp++);
            POLYUNSIGNED arg2Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg2Ptr = (*sp++).AsCodePtr();
            POLYUNSIGNED arg1Offset = UNTAGGED_UNSIGNED(*sp++);
            POLYCODEPTR arg1Ptr = (*sp).AsCodePtr();
            int result = memcmp(arg1Ptr+arg1Offset, arg2Ptr+arg2Offset, length);
            *sp = result == 0 ? TAGGED(0) : result < 0 ? TAGGED(-1) : TAGGED(1);
            break;
        }

        default: Crash("Unknown instruction %x\n", lastInstr);

        } /* switch */
     } /* for */
     return 0;
} /* MD_switch_to_poly */

void IntTaskData::GarbageCollect(ScanAddress *process)
{
    TaskData::GarbageCollect(process);

    overflowPacket = process->ScanObjectAddress(overflowPacket);
    dividePacket = process->ScanObjectAddress(dividePacket);

    if (stack != 0)
    {
        StackSpace *stackSpace = stack;
        PolyWord *stackPtr = this->sp;
        // The exception arg if any
        ScanStackAddress(process, this->exception_arg, stackSpace);

        // Now the values on the stack.
        for (PolyWord *q = stackPtr; q < stackSpace->top; q++)
            ScanStackAddress(process, *q, stackSpace);
     }
}


// Process a value within the stack.
void IntTaskData::ScanStackAddress(ScanAddress *process, PolyWord &val, StackSpace *stack)
{
    if (! val.IsDataPtr()) return;

    MemSpace *space = gMem.LocalSpaceForAddress(val.AsStackAddr()-1);
    if (space != 0)
        val = process->ScanObjectAddress(val.AsObjPtr());
}


// Copy a stack
void IntTaskData::CopyStackFrame(StackObject *old_stack, POLYUNSIGNED old_length, StackObject *new_stack, POLYUNSIGNED new_length)
{
  /* Moves a stack, updating all references within the stack */
    PolyWord *old_base  = (PolyWord *)old_stack;
    PolyWord *new_base  = (PolyWord*)new_stack;
    PolyWord *old_top   = old_base + old_length;

    /* Calculate the offset of the new stack from the old. If the frame is
       being extended objects in the new frame will be further up the stack
       than in the old one. */

    POLYSIGNED offset = new_base - old_base + new_length - old_length;
    PolyWord *oldSp = this->sp;
    this->sp    = oldSp + offset;
    this->hr    = this->hr + offset;

    /* Skip the unused part of the stack. */

    POLYUNSIGNED i = oldSp - old_base;

    ASSERT (i <= old_length);

    i = old_length - i;

    PolyWord *old = oldSp;
    PolyWord *newp= this->sp;

    while (i--)
    {
//        ASSERT(old >= old_base && old < old_base+old_length);
//        ASSERT(newp >= new_base && newp < new_base+new_length);
        PolyWord old_word = *old++;
        if (old_word.IsTagged() || old_word.AsStackAddr() < old_base || old_word.AsStackAddr() >= old_top)
            *newp++ = old_word;
        else
            *newp++ = PolyWord::FromStackAddr(old_word.AsStackAddr() + offset);
    }
    ASSERT(old == ((PolyWord*)old_stack)+old_length);
    ASSERT(newp == ((PolyWord*)new_stack)+new_length);
}

Handle IntTaskData::EnterPolyCode()
/* Called from "main" to enter the code. */
{
    Handle hOriginal = this->saveVec.mark(); // Set this up for the IO calls.
    while (1)
    {
        this->saveVec.reset(hOriginal); // Remove old RTS arguments and results.

        // Run the ML code and return with the function to call.
        this->inML = true;
        int ioFunction = SwitchToPoly();
        this->inML = false;

        try {
            switch (ioFunction)
            {
            case -1:
                // We've been interrupted.  This usually involves simulating a
                // stack overflow so we could come here because of a genuine
                // stack overflow.
                // Previously this code was executed on every RTS call but there
                // were problems on Mac OS X at least with contention on schedLock.
                // Process any asynchronous events i.e. interrupts or kill
                processes->ProcessAsynchRequests(this);
                // Release and re-acquire use of the ML memory to allow another thread
                // to GC.
                processes->ThreadReleaseMLMemory(this);
                processes->ThreadUseMLMemory(this);
                break;

            case -2: // A callback has returned.
                ASSERT(0); // Callbacks aren't implemented

            default:
                Crash("Unknown io operation %d\n", ioFunction);
            }
        }
        catch (IOException &) {
        }

    }
}

// As far as possible we want locking and unlocking an ML mutex to be fast so
// we try to implement the code in the assembly code using appropriate
// interlocked instructions.  That does mean that if we need to lock and
// unlock an ML mutex in this code we have to use the same, machine-dependent,
// code to do it.  These are defaults that are used where there is no
// machine-specific code.

static Handle ProcessAtomicIncrement(TaskData *taskData, Handle mutexp)
{
    PLocker l(&mutexLock);
    PolyObject *p = DEREFHANDLE(mutexp);
    // A thread can only call this once so the values will be short
    PolyWord newValue = TAGGED(UNTAGGED(p->Get(0))+1);
    p->Set(0, newValue);
    return taskData->saveVec.push(newValue);
}

// Release a mutex.  We need to lock the mutex to ensure we don't
// reset it in the time between one of atomic operations reading
// and writing the mutex.
static Handle ProcessAtomicReset(TaskData *taskData, Handle mutexp)
{
    PLocker l(&mutexLock);
    DEREFHANDLE(mutexp)->Set(0, TAGGED(1)); // Set this to released.
    return taskData->saveVec.push(TAGGED(0)); // Push the unit result
}

Handle IntTaskData::AtomicIncrement(Handle mutexp)
{
    return ProcessAtomicIncrement(this, mutexp);
}

void IntTaskData::AtomicReset(Handle mutexp)
{
    (void)ProcessAtomicReset(this, mutexp);
}

static Interpreter interpreterObject;

MachineDependent *machineDependent = &interpreterObject;

// PolySetCodeConstant is not actually used in the interpreted version.
// It is used in the X86 code-generator to insert inline constants.
// Compat560 creates an RTS function unconditionally and rather than change
// that it's easier to add it here for the time being.
extern "C" {
    POLYEXTERNALSYMBOL POLYUNSIGNED PolySetCodeConstant(byte *pointer, PolyWord offset, POLYUNSIGNED c, PolyWord flags);
}

POLYUNSIGNED PolySetCodeConstant(byte *pointer, PolyWord offset, POLYUNSIGNED c, PolyWord flags)
{
    return TAGGED(0).AsUnsigned();
}

struct _entrypts machineSpecificEPT[] =
{
    { "PolySetCodeConstant",              (polyRTSFunction)&PolySetCodeConstant},

    { NULL, NULL} // End of list.
};
