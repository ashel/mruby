/*
** vm.c - virtual machine for mruby (RiteVM)
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "opcode.h"
#include "mruby/irep.h"
#include "mruby/variable.h"
#include "mruby/proc.h"
#include "mruby/array.h"
#include "mruby/string.h"
#include "mruby/hash.h"
#include "mruby/range.h"
#include "mruby/class.h"
#include "mruby/numeric.h"
#include "error.h"

#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#define STACK_INIT_SIZE 128
#define CALLINFO_INIT_SIZE 32

static void
stack_init(mrb_state *mrb)
{
  /* assert(mrb->stack == NULL); */
  mrb->stbase = mrb_malloc(mrb, sizeof(mrb_value) * STACK_INIT_SIZE);
  memset(mrb->stbase, 0, sizeof(mrb_value) * STACK_INIT_SIZE);
  mrb->stend = mrb->stbase + STACK_INIT_SIZE;
  mrb->stack = mrb->stbase;

  /* assert(mrb->ci == NULL); */
  mrb->cibase = mrb_malloc(mrb, sizeof(mrb_callinfo)*CALLINFO_INIT_SIZE);
  mrb->ciend = mrb->cibase + CALLINFO_INIT_SIZE;
  mrb->ci = mrb->cibase;
  memset(mrb->ci, 0, sizeof(mrb_callinfo));
  mrb->ci->target_class = mrb->object_class;
}

static void
stack_extend(mrb_state *mrb, int room, int keep)
{
  int size, off;

  if (mrb->stack + room > mrb->stend) {
    size = mrb->stend - mrb->stbase;
    off = mrb->stack - mrb->stbase;

    if (room <= size)  /* double size is enough? */
      size *= 2;
    else
      size += room;
    mrb->stbase = mrb_realloc(mrb, mrb->stbase, sizeof(mrb_value) * size);
    mrb->stack = mrb->stbase + off;
    mrb->stend = mrb->stbase + size;
  }
  if (room > keep) {
    memset(mrb->stack+keep, 0, sizeof(mrb_value) * (room-keep));
  }
}

int
mrb_checkstack(mrb_state *mrb, int size)
{
  stack_extend(mrb, size+1, 1);
  return 0;
}

struct REnv*
uvenv(mrb_state *mrb, int up)
{
  struct REnv *e = mrb->ci->proc->env;

  if (!e) return 0;
  while (up--) {
    e = (struct REnv*)e->c;
  }
  return e;
}

static mrb_value
uvget(mrb_state *mrb, int up, int idx)
{
  struct REnv *e = uvenv(mrb, up);

  if (!e) return mrb_nil_value();
  return e->stack[idx];
}

static void
uvset(mrb_state *mrb, int up, int idx, mrb_value v)
{
  struct REnv *e = uvenv(mrb, up);

  if (!e) return;
  e->stack[idx] = v;
  mrb_write_barrier(mrb, (struct RBasic*)e);
}

static mrb_callinfo*
cipush(mrb_state *mrb)
{
  size_t nregs = mrb->ci->nregs;
  int eidx = mrb->ci->eidx;
  int ridx = mrb->ci->ridx;

  if (mrb->ci + 1 == mrb->ciend) {
    size_t size = mrb->ci - mrb->cibase;

    mrb->cibase = mrb_realloc(mrb, mrb->cibase, sizeof(mrb_callinfo)*size*2);
    mrb->ci = mrb->cibase + size;
    mrb->ciend = mrb->cibase + size * 2;
  }
  mrb->ci++;
  mrb->ci->nregs = nregs;
  mrb->ci->eidx = eidx;
  mrb->ci->ridx = ridx;
  mrb->ci->env = 0;
  return mrb->ci;
}

static void
cipop(mrb_state *mrb)
{
  mrb->ci--;
}

static void
ecall(mrb_state *mrb, int i)
{
  struct RProc *p;
  mrb_callinfo *ci;
  mrb_value *self = mrb->stack;

  p = mrb->ensure[i];
  ci = cipush(mrb);
  ci->stackidx = mrb->stack - mrb->stbase;
  ci->mid = ci[-1].mid;
  ci->acc = -1;
  ci->argc = 0;
  ci->proc = p;
  ci->nregs = p->body.irep->nregs;
  ci->target_class = p->target_class;
  mrb->stack = mrb->stack + ci[-1].nregs;
  mrb_run(mrb, p, *self);
}

mrb_value
mrb_funcall_with_block(mrb_state *mrb, mrb_value self, const char *name, int argc, mrb_value *argv, struct RProc *blk)
{
  struct RProc *p;
  struct RClass *c;
  mrb_sym mid = mrb_intern(mrb, name);
  mrb_sym undef = 0;
  mrb_callinfo *ci;
  int n = mrb->ci->nregs;
  mrb_value val;

  c = mrb_class(mrb, self);
  p = mrb_method_search_vm(mrb, &c, mid);
  if (!p) {
    undef = mid;
    mid = mrb_intern(mrb, "method_missing");
    p = mrb_method_search_vm(mrb, &c, mid);
    n++; argc++;
  }
  ci = cipush(mrb);
  ci->mid = mid;
  ci->proc = p;
  ci->stackidx = mrb->stack - mrb->stbase;
  ci->argc = argc;
  ci->target_class = p->target_class;
  ci->nregs = argc + 2;
  ci->acc = -1;
  mrb->stack = mrb->stack + n;

  stack_extend(mrb, ci->nregs, 0);
  mrb->stack[0] = self;
  if (undef) {
    mrb->stack[1] = mrb_symbol_value(undef);
    memcpy(mrb->stack+2, argv, sizeof(mrb_value)*(argc-1));
  }
  else if (argc > 0) {
    memcpy(mrb->stack+1, argv, sizeof(mrb_value)*argc);
  }
  if (!blk) {
    mrb->stack[argc+1] = mrb_nil_value();
  }
  else {
    mrb->stack[argc+1] = mrb_obj_value(blk);
  }

  if (MRB_PROC_CFUNC_P(p)) {
    val = p->body.func(mrb, self);
    mrb->stack = mrb->stbase + ci->stackidx;
    cipop(mrb);
  }
  else {
    val = mrb_run(mrb, p, self);
  }
  return val;
}

mrb_value
mrb_funcall_argv(mrb_state *mrb, mrb_value self, const char *name, int argc, mrb_value *argv)
{
  return mrb_funcall_with_block(mrb, self, name, argc, argv, 0);
}

mrb_value
mrb_yield_with_self(mrb_state *mrb, mrb_value b, int argc, mrb_value *argv, mrb_value self)
{
  struct RProc *p;
  mrb_sym mid = mrb->ci->mid;
  mrb_callinfo *ci;
  int n = mrb->ci->nregs;
  mrb_value val;

  p = mrb_proc_ptr(b);
  ci = cipush(mrb);
  ci->mid = mid;
  ci->proc = p;
  ci->stackidx = mrb->stack - mrb->stbase;
  ci->argc = argc;
  ci->target_class = p->target_class;
  ci->nregs = argc + 2;
  ci->acc = -1;
  mrb->stack = mrb->stack + n;

  stack_extend(mrb, ci->nregs, 0);
  mrb->stack[0] = self;
  if (argc > 0) {
    memcpy(mrb->stack+1, argv, sizeof(mrb_value)*argc);
  }
  mrb->stack[argc+1] = mrb_nil_value();

  if (MRB_PROC_CFUNC_P(p)) {
    val = p->body.func(mrb, self);
    mrb->stack = mrb->stbase + ci->stackidx;
    cipop(mrb);
  }
  else {
    val = mrb_run(mrb, p, self);
  }
  return val;
}

mrb_value
mrb_yield_argv(mrb_state *mrb, mrb_value b, int argc, mrb_value *argv)
{
  return mrb_yield_with_self(mrb, b, argc, argv, mrb->stack[0]);
}

mrb_value
mrb_yield(mrb_state *mrb, mrb_value b, mrb_value v)
{
  return mrb_yield_with_self(mrb, b, 1, &v, mrb->stack[0]);
}

static void
localjump_error(mrb_state *mrb, const char *kind)
{
  char buf[256];
  mrb_value exc;

  snprintf(buf, 256, "unexpected %s", kind);
  exc = mrb_exc_new(mrb, E_LOCALJUMP_ERROR, buf, strlen(buf));
  mrb->exc = mrb_object(exc);
}

static void
argnum_error(mrb_state *mrb, int num)
{
  char buf[256];
  mrb_value exc;

  if (mrb->ci->mid) {
    snprintf(buf, 256, "'%s': wrong number of arguments (%d for %d)",
	     mrb_sym2name(mrb, mrb->ci->mid),
	     mrb->ci->argc, num);
  }
  else {
    snprintf(buf, 256, "wrong number of arguments (%d for %d)",
	     mrb->ci->argc, num);
  }
  exc = mrb_exc_new(mrb, E_ARGUMENT_ERROR, buf, strlen(buf));
  mrb->exc = mrb_object(exc);
}

#define SET_TRUE_VALUE(r) {\
  (r).tt = MRB_TT_TRUE;\
  (r).value.i = 1;\
}

#define SET_FALSE_VALUE(r) {\
  (r).tt = MRB_TT_FALSE;\
  (r).value.i = 1;\
}

#define SET_NIL_VALUE(r) { \
  (r).tt = MRB_TT_FALSE;\
  (r).value.p = 0;\
}

#define SET_INT_VALUE(r,n) {\
  (r).tt = MRB_TT_FIXNUM;\
  (r).value.i = (n);\
}

#define SET_FLOAT_VALUE(r,v) {\
  (r).tt = MRB_TT_FLOAT;\
  (r).value.f = (v);\
}

#define SET_SYM_VALUE(r,v) {\
  (r).tt = MRB_TT_SYMBOL;\
  (r).value.sym = (v);\
}

#define SET_OBJ_VALUE(r,v) {\
  (r).tt = (((struct RObject*)(v))->tt);\
  (r).value.p = (void*)(v);\
}

#define DIRECT_THREADED
#ifndef DIRECT_THREADED

#define INIT_DISPACTH for (;;) { i = *pc; switch (GET_OPCODE(i)) {
#define CASE(op) case op:
#define NEXT mrb->arena_idx = ai; pc++; break
#define JUMP break
#define END_DISPACTH }}

#else

#define INIT_DISPACTH JUMP; return mrb_nil_value();
#define CASE(op) L_ ## op:
#define NEXT mrb->arena_idx = ai; i=*++pc; goto *optable[GET_OPCODE(i)]
#define JUMP i=*pc; goto *optable[GET_OPCODE(i)]

#define END_DISPACTH

#endif

mrb_value mrb_gv_val_get(mrb_state *mrb, mrb_sym sym);
void mrb_gv_val_set(mrb_state *mrb, mrb_sym sym, mrb_value val);

#define CALL_MAXARGS 127

mrb_value
mrb_run(mrb_state *mrb, struct RProc *proc, mrb_value self)
{
  /* assert(mrb_proc_cfunc_p(proc)) */
  mrb_irep *irep = proc->body.irep;
  mrb_code *pc = irep->iseq;
  mrb_value *pool = irep->pool;
  mrb_sym *syms = irep->syms;
  mrb_value *regs;
  mrb_code i;
  int ai = mrb->arena_idx;
  jmp_buf c_jmp;
  jmp_buf *prev_jmp;

#ifdef DIRECT_THREADED
  static void *optable[] = {
    &&L_OP_NOP, &&L_OP_MOVE,
    &&L_OP_LOADL, &&L_OP_LOADI, &&L_OP_LOADSYM, &&L_OP_LOADNIL,
    &&L_OP_LOADSELF, &&L_OP_LOADT, &&L_OP_LOADF,
    &&L_OP_GETGLOBAL, &&L_OP_SETGLOBAL, &&L_OP_GETSPECIAL, &&L_OP_SETSPECIAL,
    &&L_OP_GETIV, &&L_OP_SETIV, &&L_OP_GETCV, &&L_OP_SETCV,
    &&L_OP_GETCONST, &&L_OP_SETCONST, &&L_OP_GETMCNST, &&L_OP_SETMCNST,
    &&L_OP_GETUPVAR, &&L_OP_SETUPVAR,
    &&L_OP_JMP, &&L_OP_JMPIF, &&L_OP_JMPNOT,
    &&L_OP_ONERR, &&L_OP_RESCUE, &&L_OP_POPERR, &&L_OP_RAISE, &&L_OP_EPUSH, &&L_OP_EPOP,
    &&L_OP_SEND, &&L_OP_FSEND, &&L_OP_VSEND,
    &&L_OP_CALL, &&L_OP_SUPER, &&L_OP_ARGARY, &&L_OP_ENTER,
    &&L_OP_KARG, &&L_OP_KDICT, &&L_OP_RETURN, &&L_OP_TAILCALL, &&L_OP_BLKPUSH,
    &&L_OP_ADD, &&L_OP_ADDI, &&L_OP_SUB, &&L_OP_SUBI, &&L_OP_MUL, &&L_OP_DIV,
    &&L_OP_EQ, &&L_OP_LT, &&L_OP_LE, &&L_OP_GT, &&L_OP_GE,
    &&L_OP_ARRAY, &&L_OP_ARYCAT, &&L_OP_ARYPUSH, &&L_OP_AREF, &&L_OP_ASET, &&L_OP_APOST,
    &&L_OP_STRING, &&L_OP_STRCAT, &&L_OP_HASH,
    &&L_OP_LAMBDA, &&L_OP_RANGE, &&L_OP_OCLASS,
    &&L_OP_CLASS, &&L_OP_MODULE, &&L_OP_EXEC,
    &&L_OP_METHOD, &&L_OP_SCLASS, &&L_OP_TCLASS,
    &&L_OP_DEBUG, &&L_OP_STOP, &&L_OP_ERR,
  };
#endif


  if (setjmp(c_jmp) == 0) {
    prev_jmp = mrb->jmp;
    mrb->jmp = &c_jmp;
  }
  else {
    goto L_RAISE;
  }
  if (!mrb->stack) {
    stack_init(mrb);
  }
  mrb->ci->proc = proc;
  mrb->ci->nregs = irep->nregs + 2;
  regs = mrb->stack;

  INIT_DISPACTH {
    CASE(OP_NOP) {
      /* do nothing */
      NEXT;
    }

    CASE(OP_MOVE) {
      /* A B    R(A) := R(B) */
#if 0
      regs[GETARG_A(i)] = regs[GETARG_B(i)];
#elif 1
      int a = GETARG_A(i);
      int b = GETARG_B(i);

      regs[a].tt = regs[b].tt;
      regs[a].value = regs[b].value;
#else
      memcpy(regs+GETARG_A(i), regs+GETARG_B(i), sizeof(mrb_value));
#endif
      NEXT;
    }

    CASE(OP_LOADL) {
      /* A Bx   R(A) := Pool(Bx) */
      regs[GETARG_A(i)] = pool[GETARG_Bx(i)];
      NEXT;
    }

    CASE(OP_LOADI) {
      /* A Bx   R(A) := sBx */
      SET_INT_VALUE(regs[GETARG_A(i)], GETARG_sBx(i));
      NEXT;
    }

    CASE(OP_LOADSYM) {
      /* A B    R(A) := Sym(B) */
      SET_SYM_VALUE(regs[GETARG_A(i)], syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_LOADNIL) {
      /* A B    R(A) := nil */
      int a = GETARG_A(i);

      SET_NIL_VALUE(regs[a]);
      NEXT;
    }

    CASE(OP_LOADSELF) {
      /* A      R(A) := self */
      regs[GETARG_A(i)] = mrb->stack[0];
      NEXT;
    }

    CASE(OP_LOADT) {
      /* A      R(A) := true */
      regs[GETARG_A(i)] = mrb_true_value();
      NEXT;
    }

    CASE(OP_LOADF) {
      /* A      R(A) := false */
      regs[GETARG_A(i)] = mrb_false_value();
      NEXT;
    }

    CASE(OP_GETGLOBAL) {
      /* A B    R(A) := getglobal(Sym(B)) */
      regs[GETARG_A(i)] = mrb_gv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETGLOBAL) {
      /* setglobal(Sym(b), R(A)) */
      mrb_gv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETSPECIAL) {
      /* A Bx   R(A) := Special[Bx] */
      regs[GETARG_A(i)] = mrb_vm_special_get(mrb, GETARG_Bx(i));
      NEXT;
    }

    CASE(OP_SETSPECIAL) {
      /* A Bx   Special[Bx] := R(A) */
      mrb_vm_special_set(mrb, GETARG_Bx(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETIV) {
      /* A Bx   R(A) := ivget(Bx) */
      regs[GETARG_A(i)] = mrb_vm_iv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETIV) {
      /* ivset(Sym(B),R(A)) */
      mrb_vm_iv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETCV) {
      /* A B    R(A) := ivget(Sym(B)) */
      regs[GETARG_A(i)] = mrb_vm_cv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETCV) {
      /* ivset(Sym(B),R(A)) */
      mrb_vm_cv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETCONST) {
      /* A B    R(A) := constget(Sym(B)) */
      regs[GETARG_A(i)] = mrb_vm_const_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETCONST) {
      /* A B    constset(Sym(B),R(A)) */
      mrb_vm_const_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETMCNST) {
      /* A B C  R(A) := R(C)::Sym(B) */
      int a = GETARG_A(i);

      regs[a] = mrb_const_get(mrb, regs[a], syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETMCNST) {
      /* A B C  R(A+1)::Sym(B) := R(A) */
      int a = GETARG_A(i);

      mrb_const_set(mrb, regs[a+1], syms[GETARG_Bx(i)], regs[a]);
      NEXT;
    }

    CASE(OP_GETUPVAR) {
      /* A B C  R(A) := uvget(B,C) */
      regs[GETARG_A(i)] = uvget(mrb, GETARG_C(i), GETARG_B(i));
      NEXT;
    }

    CASE(OP_SETUPVAR) {
      /* A B C  uvset(B,C,R(A)) */
      uvset(mrb, GETARG_C(i), GETARG_B(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_JMP) {
      /* sBx    pc+=sBx */
      pc += GETARG_sBx(i);
      JUMP;
    }

    CASE(OP_JMPIF) {
      /* A sBx  if R(A) pc+=sBx */
      if (mrb_test(regs[GETARG_A(i)])) {
        pc += GETARG_sBx(i);
        JUMP;
      }
      NEXT;
    }

    CASE(OP_JMPNOT) {
      /* A sBx  if R(A) pc+=sBx */
      if (!mrb_test(regs[GETARG_A(i)])) {
        pc += GETARG_sBx(i);
        JUMP;
      }
      NEXT;
    }

    CASE(OP_ONERR) {
      /* sBx    pc+=sBx on exception */
      if (mrb->rsize <= mrb->ci->ridx) {
        if (mrb->rsize == 0) mrb->rsize = 16;
        else mrb->rsize *= 2;
        mrb->rescue = mrb_realloc(mrb, mrb->rescue, sizeof(mrb_code*) * mrb->rsize);
      }
      mrb->rescue[mrb->ci->ridx++] = pc + GETARG_sBx(i);
      NEXT;
    }

    CASE(OP_RESCUE) {
      /* A      R(A) := exc; clear(exc) */
      SET_OBJ_VALUE(regs[GETARG_A(i)],mrb->exc);
      mrb->exc = 0;
      NEXT;
    }

    CASE(OP_POPERR) {
      int a = GETARG_A(i);

      while (a--) {
        mrb->ci->ridx--;
      }
      NEXT;
    }

    CASE(OP_RAISE) {
      /* A      raise(R(A)) */
      mrb->exc = mrb_object(regs[GETARG_A(i)]);
      goto L_RAISE;
    }

    CASE(OP_EPUSH) {
      /* Bx     ensure_push(SEQ[Bx]) */
      struct RProc *p;

      p = mrb_closure_new(mrb, mrb->irep[irep->idx+GETARG_Bx(i)]);
      /* push ensure_stack */
      if (mrb->esize <= mrb->ci->eidx) {
        if (mrb->esize == 0) mrb->esize = 16;
        else mrb->esize *= 2;
        mrb->ensure = mrb_realloc(mrb, mrb->ensure, sizeof(struct RProc*) * mrb->esize);
      }
      mrb->ensure[mrb->ci->eidx++] = p;
      NEXT;
    }

    CASE(OP_EPOP) {
      /* A      A.times{ensure_pop().call} */
      int n;
      int a = GETARG_A(i);

      for (n=0; n<a; n++) {
        ecall(mrb, --mrb->ci->eidx);
      }
      NEXT;
    }

  L_SEND:
    CASE(OP_SEND) {
      /* A B C  R(A) := call(R(A),Sym(B),R(A+1),... ,R(A+C-1)) */
      int a = GETARG_A(i);
      int n = GETARG_C(i);
      struct RProc *m;
      struct RClass *c;
      mrb_callinfo *ci;
      mrb_value recv;
      mrb_sym mid = syms[GETARG_B(i)];

      recv = regs[a];
      c = mrb_class(mrb, recv);
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mrb_value sym = mrb_symbol_value(mid);

        mid = mrb_intern(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], sym);
        }
        else {
          memmove(regs+a+2, regs+a+1, sizeof(mrb_value)*(n+1));
          regs[a+1] = sym;
          n++;
        }
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->proc = m;
      ci->stackidx = mrb->stack - mrb->stbase;
      ci->argc = n;
      if (ci->argc == CALL_MAXARGS) ci->argc = -1;
      ci->target_class = m->target_class;
      ci->pc = pc + 1;

      /* prepare stack */
      mrb->stack += a;

      if (MRB_PROC_CFUNC_P(m)) {
        mrb->stack[0] = m->body.func(mrb, recv);
        mrb->arena_idx = ai;
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        mrb->stack = mrb->stbase + ci->stackidx;
        cipop(mrb);
        NEXT;
      }
      else {
        /* fill callinfo */
        ci->acc = a;

        /* setup environment for calling method */
        proc = mrb->ci->proc = m;
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_FSEND) {
      /* A B C  R(A) := fcall(R(A),Sym(B),R(A+1),... ,R(A+C)) */
      NEXT;
    }

    CASE(OP_VSEND) {
      /* A B    R(A) := vcall(R(A),Sym(B)) */
      NEXT;
    }

    CASE(OP_CALL) {
      /* A      R(A) := self.call(frame.argc, frame.argv) */
      mrb_callinfo *ci;
      mrb_value recv = mrb->stack[0];
      struct RProc *m = mrb_proc_ptr(recv);

      /* replace callinfo */
      ci = mrb->ci;
      ci->target_class = m->target_class;
      ci->proc = m;
      if (m->env) {
	if (m->env->mid) {
	  ci->mid = m->env->mid;
	}
        if (!m->env->stack) {
          m->env->stack = mrb->stack;
        }
      }

      /* prepare stack */
      if (MRB_PROC_CFUNC_P(m)) {
        mrb->stack[0] = m->body.func(mrb, recv);
        mrb->arena_idx = ai;
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        regs = mrb->stack = mrb->stbase + ci->stackidx;
        cipop(mrb);
        NEXT;
      }
      else {
        /* setup environment for calling method */
        proc = m;
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        regs[0] = m->env->stack[0];
        pc = m->body.irep->iseq;
        JUMP;
      }
    }

    CASE(OP_SUPER) {
      /* A B C  R(A) := super(R(A+1),... ,R(A+C-1)) */
      mrb_value recv;
      mrb_callinfo *ci = mrb->ci;
      struct RProc *m;
      struct RClass *c;
      mrb_sym mid = ci->mid;
      int a = GETARG_A(i);
      int n = GETARG_C(i);

      recv = regs[0];
      c = mrb->ci->proc->target_class->super;
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        c = mrb->ci->proc->target_class;
        mid = mrb_intern(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], mrb_symbol_value(ci->mid));
        }
        else {
          memmove(regs+a+2, regs+a+1, sizeof(mrb_value)*(n+1));
          regs[a+1] = mrb_symbol_value(ci->mid);
          n++;
        }
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->proc = m;
      ci->stackidx = mrb->stack - mrb->stbase;
      ci->argc = n;
      if (ci->argc == CALL_MAXARGS) ci->argc = -1;
      ci->target_class = m->target_class;
      ci->pc = pc + 1;

      /* prepare stack */
      mrb->stack += a;
      mrb->stack[0] = recv;

      if (MRB_PROC_CFUNC_P(m)) {
        mrb->stack[0] = m->body.func(mrb, recv);
        mrb->arena_idx = ai;
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        mrb->stack = mrb->stbase + ci->stackidx;
        cipop(mrb);
        NEXT;
      }
      else {
        /* fill callinfo */
        ci->acc = a;

        /* setup environment for calling method */
        ci->proc = m;
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_ARGARY) {
      /* A Bx   R(A) := argument array (16=6:1:5:4) */
      int a = GETARG_A(i);
      int bx = GETARG_Bx(i);
      int m1 = (bx>>10)&0x3f;
      int r  = (bx>>9)&0x1;
      int m2 = (bx>>4)&0x1f;
      int lv = (bx>>0)&0xf;
      mrb_value *stack;

      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
        stack = e->stack + 1;
      }
      if (r == 0) {
        regs[a] = mrb_ary_new_elts(mrb, m1+m2, stack);
      }
      else {
        mrb_value *pp;
        struct RArray *rest;
        int len = 0;

        if (stack[m1].tt == MRB_TT_ARRAY) {
          struct RArray *ary = mrb_ary_ptr(stack[m1]);

          pp = ary->buf;
          len = ary->len;
        }
        regs[a] = mrb_ary_new_capa(mrb, m1+len+m2);
        rest = mrb_ary_ptr(regs[a]);
        memcpy(rest->buf, stack, sizeof(mrb_value)*m1);
        if (len > 0) {
          memcpy(rest->buf+m1, pp, sizeof(mrb_value)*len);
        }
        if (m2 > 0) {
          memcpy(rest->buf+m1+len, stack+m1+1, sizeof(mrb_value)*m2);
        }
        rest->len = m1+len+m2;
      }
      regs[a+1] = stack[m1+r+m2];
      NEXT;
    }

    CASE(OP_ENTER) {
      /* Ax             arg setup according to flags (24=5:5:1:5:5:1:1) */
      /* number of optional arguments times OP_JMP should follow */
      int ax = GETARG_Ax(i);
      int m1 = (ax>>18)&0x1f;
      int o  = (ax>>13)&0x1f;
      int r  = (ax>>12)&0x1;
      int m2 = (ax>>7)&0x1f;
      /* unused
      int k  = (ax>>2)&0x1f;
      int kd = (ax>>1)&0x1;
      int b  = (ax>>0)& 0x1;
      */
      int argc = mrb->ci->argc;
      mrb_value *argv = regs+1;
      int len = m1 + o + r + m2;

      if (argc < 0) {
        struct RArray *ary = mrb_ary_ptr(regs[1]);
        argv = ary->buf;
        argc = ary->len;
        regs[len+2] = regs[1];  /* save argary in register */
      }
      if (mrb->ci->proc && MRB_PROC_STRICT_P(mrb->ci->proc)) {
        if (argc >= 0) {
          if (argc < m1 + m2 || (r == 0 && argc > len)) {
	    argnum_error(mrb, m1+m2);
	    goto L_RAISE;
          }
        }
      }
      else if (len > 1 && argc == 1 && argv[0].tt == MRB_TT_ARRAY) {
        argc = mrb_ary_ptr(argv[0])->len;
        argv = mrb_ary_ptr(argv[0])->buf;
      }
      mrb->ci->argc = len;
      if (argc < len) {
        regs[len+1] = argv[argc]; /* move block */
        memmove(&regs[1], argv, sizeof(mrb_value)*(argc-m2)); /* m1 + o */
        memmove(&regs[len-m2+1], &argv[argc-m2], sizeof(mrb_value)*m2); /* m2 */
        if (r) {                  /* r */
          regs[m1+o+1] = mrb_ary_new_capa(mrb, 0);
        }
        pc += argc - m1 - m2 + 1;
      }
      else {
        memmove(&regs[1], argv, sizeof(mrb_value)*(m1+o)); /* m1 + o */
        if (r) {                  /* r */
          regs[m1+o+1] = mrb_ary_new_elts(mrb, argc-m1-o-m2, argv+m1+o);
        }
        memmove(&regs[m1+o+r+1], &argv[argc-m2], sizeof(mrb_value)*m2);
        regs[len+1] = argv[argc]; /* move block */
        pc += o + 1;
      }
      JUMP;
    }

    CASE(OP_KARG) {
      /* A B C          R(A) := kdict[Sym(B)]; if C kdict.rm(Sym(B)) */
      /* if C == 2; raise unless kdict.empty? */
      /* OP_JMP should follow to skip init code */
      NEXT;
    }

    CASE(OP_KDICT) {
      /* A C            R(A) := kdict */
      NEXT;
    }

    CASE(OP_RETURN) {
      /* A      return R(A) */
    L_RETURN:
      if (mrb->ci->env) {
        struct REnv *e = mrb->ci->env;
        int len = (int)e->flags;
        mrb_value *p = mrb_malloc(mrb, sizeof(mrb_value)*len);

        e->cioff = -1;
        memcpy(p, e->stack, sizeof(mrb_value)*len);
        e->stack = p;
      }

      if (mrb->exc) {
        mrb_callinfo *ci;

      L_RAISE:
        ci = mrb->ci;
        if (ci == mrb->cibase) goto L_STOP;
        while (ci[0].ridx == ci[-1].ridx) {
          cipop(mrb);
          ci = mrb->ci;
          if (ci == mrb->cibase) {
            if (ci->ridx == 0) goto L_STOP;
            break;
          }
        }
        irep = ci->proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        regs = mrb->stack = mrb->stbase + ci->stackidx;
        pc = mrb->rescue[--ci->ridx];
      }
      else {
        mrb_callinfo *ci = mrb->ci;
        int acc, eidx = mrb->ci->eidx;
        mrb_value v = regs[GETARG_A(i)];

        switch (GETARG_B(i)) {
        case OP_R_NORMAL:
          ci = mrb->ci;
          break;
        case OP_R_BREAK:
          if (proc->env->cioff < 0) {
            localjump_error(mrb, "break");
            goto L_RAISE;
          }
          ci = mrb->ci = mrb->cibase + proc->env->cioff + 1;
          break;
        case OP_R_RETURN:
          if (proc->env->cioff < 0) {
            localjump_error(mrb, "return");
            goto L_RAISE;
          }
          ci = mrb->ci = mrb->cibase + proc->env->cioff;
          break;
        default:
          /* cannot happen */
          break;
        }
        cipop(mrb);
        acc = ci->acc;
        pc = ci->pc;
        regs = mrb->stack = mrb->stbase + ci->stackidx;
        while (eidx > mrb->ci->eidx) {
          ecall(mrb, --eidx);
        }
        if (acc < 0) {
          mrb->jmp = prev_jmp;
          return v;
        }
        DEBUG(printf("from :%s\n", mrb_sym2name(mrb, ci->mid)));
        proc = mrb->ci->proc;
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;

        regs[acc] = v;
      }
      JUMP;
    }

    CASE(OP_TAILCALL) {
      /* A B C  return call(R(A),Sym(B),R(A+1),... ,R(A+C-1)) */
      int a = GETARG_A(i);
      int n = GETARG_C(i);
      struct RProc *m;
      struct RClass *c;
      mrb_callinfo *ci;
      mrb_value recv;
      mrb_sym mid = syms[GETARG_B(i)];

      recv = regs[a];
      c = mrb_class(mrb, recv);
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mrb_value sym = mrb_symbol_value(mid);

        mid = mrb_intern(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], sym);
        }
        else {
          memmove(regs+a+2, regs+a+1, sizeof(mrb_value)*(n+1));
          regs[a+1] = sym;
          n++;
        }
      }


      /* replace callinfo */
      mrb->ci = ci = &mrb->ci[-1];
      ci->mid = mid;
      ci->target_class = m->target_class;
      ci->argc = n;
      if (ci->argc == CALL_MAXARGS) ci->argc = -1;

      /* move stack */
      memmove(mrb->stack, &regs[a], (ci->argc+1)*sizeof(mrb_value));

      if (MRB_PROC_CFUNC_P(m)) {
        mrb->stack[0] = m->body.func(mrb, recv);
        mrb->arena_idx = ai;
        goto L_RETURN;
      }
      else {
        /* setup environment for calling method */
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        pc = irep->iseq;
      }
      JUMP;
    }

    CASE(OP_BLKPUSH) {
      /* A Bx   R(A) := block (16=6:1:5:4) */
      int a = GETARG_A(i);
      int bx = GETARG_Bx(i);
      int m1 = (bx>>10)&0x3f;
      int r  = (bx>>9)&0x1;
      int m2 = (bx>>4)&0x1f;
      int lv = (bx>>0)&0xf;
      mrb_value *stack;

      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
        stack = e->stack + 1;
      }
      regs[a] = stack[m1+r+m2];
      NEXT;
    }

#define TYPES2(a,b) (((((int)(a))<<8)|((int)(b)))&0xffff)
#define OP_MATH_BODY(op,v1,v2) do {\
  regs[a].value.v1 = regs[a].value.v1 op regs[a+1].value.v2;\
} while(0)

#define OP_MATH(op) do {\
  int a = GETARG_A(i);\
  /* need to check if - is overridden */\
  switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):\
    OP_MATH_BODY(op,i,i);                  \
    break;\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):\
    {\
      mrb_int x = regs[a].value.i;\
      mrb_float y = regs[a+1].value.f;\
      SET_FLOAT_VALUE(regs[a], (mrb_float)x op y);\
    }\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):\
    OP_MATH_BODY(op,f,i);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):\
    OP_MATH_BODY(op,f,f);\
    break;\
  default:\
    i = MKOP_ABC(OP_SEND, a, GETARG_B(i), GETARG_C(i));\
    goto L_SEND;\
  }\
} while (0)

    CASE(OP_ADD) {
      /* A B C  R(A) := R(A)+R(A+1) (Syms[B]=:+,C=1)*/
      int a = GETARG_A(i);

      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
        OP_MATH_BODY(+,i,i);
        break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
        {
          mrb_int x = regs[a].value.i;
          mrb_float y = regs[a+1].value.f;
          SET_FLOAT_VALUE(regs[a], (mrb_float)x + y);
        }
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
        OP_MATH_BODY(+,f,i);
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
        OP_MATH_BODY(+,f,f);
        break;
      case TYPES2(MRB_TT_STRING,MRB_TT_STRING):
        regs[a] = mrb_str_plus(mrb, regs[a], regs[a+1]);
        break;
      default:
        i = MKOP_ABC(OP_SEND, a, GETARG_B(i), GETARG_C(i));
        goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_SUB) {
      /* A B C  R(A) := R(A)-R(A+1) (Syms[B]=:-,C=1)*/
      OP_MATH(-);
      NEXT;
    }

    CASE(OP_MUL) {
      /* A B C  R(A) := R(A)*R(A+1) (Syms[B]=:*,C=1)*/
      OP_MATH(*);
      NEXT;
    }

    CASE(OP_DIV) {
      /* A B C  R(A) := R(A)/R(A+1) (Syms[B]=:/,C=1)*/
      OP_MATH(/);
      NEXT;
    }

    CASE(OP_ADDI) {
      /* A B C  R(A) := R(A)+C (Syms[B]=:+)*/
      int a = GETARG_A(i);

      /* need to check if + is overridden */
      switch (mrb_type(regs[a])) {
      case MRB_TT_FIXNUM:
        regs[a].value.i += GETARG_C(i);
        break;
      case MRB_TT_FLOAT:
        regs[a].value.f += GETARG_C(i);
        break;
      default:
        SET_INT_VALUE(regs[a+1], GETARG_C(i));
        i = MKOP_ABC(OP_SEND, a, GETARG_B(i), 1);
        goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_SUBI) {
      /* A B C  R(A) := R(A)-C (Syms[B]=:+)*/
      int a = GETARG_A(i);

      /* need to check if + is overridden */
      switch (mrb_type(regs[a])) {
      case MRB_TT_FIXNUM:
        regs[a].value.i -= GETARG_C(i);
        break;
      case MRB_TT_FLOAT:
        regs[a].value.f -= GETARG_C(i);
        break;
      default:
        SET_INT_VALUE(regs[a+1], GETARG_C(i));
        i = MKOP_ABC(OP_SEND, a, GETARG_B(i), 1);
        goto L_SEND;
      }
      NEXT;
    }

#define OP_CMP_BODY(op,v1,v2) do {\
  if (regs[a].value.v1 op regs[a+1].value.v2) {\
    SET_TRUE_VALUE(regs[a]);\
  }\
  else {\
    SET_FALSE_VALUE(regs[a]);\
  }\
} while(0)

#define OP_CMP(op) do {\
  int a = GETARG_A(i);\
  /* need to check if - is overridden */\
  switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):\
    OP_CMP_BODY(op,i,i);                   \
    break;\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):\
    OP_CMP_BODY(op,i,f);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):\
    OP_CMP_BODY(op,f,i);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):\
    OP_CMP_BODY(op,f,f);\
    break;\
  default:\
    i = MKOP_ABC(OP_SEND, a, GETARG_B(i), GETARG_C(i));\
    goto L_SEND;\
  }\
} while (0)

    CASE(OP_EQ) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(==);
      NEXT;
    }

    CASE(OP_LT) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(<);
      NEXT;
    }

    CASE(OP_LE) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(<=);
      NEXT;
    }

    CASE(OP_GT) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(>);
      NEXT;
    }

    CASE(OP_GE) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(>=);
      NEXT;
    }

    CASE(OP_ARRAY) {
      /* A B C          R(A) := ary_new(R(B),R(B+1)..R(B+C)) */
      int b = GETARG_B(i);
      int lim = b+GETARG_C(i);
      mrb_value ary = mrb_ary_new_capa(mrb, GETARG_C(i));

      while (b < lim) {
        mrb_ary_push(mrb, ary, regs[b++]);
      }
      regs[GETARG_A(i)] = ary;
      NEXT;
    }

    CASE(OP_ARYCAT) {
      /* A B            mrb_ary_concat(R(A),R(B)) */
      mrb_ary_concat(mrb, regs[GETARG_A(i)],
                     mrb_ary_splat(mrb, regs[GETARG_B(i)]));
      NEXT;
    }

    CASE(OP_ARYPUSH) {
      /* A B            R(A).push(R(B)) */
      mrb_ary_push(mrb, regs[GETARG_A(i)], regs[GETARG_B(i)]);
      NEXT;
    }

    CASE(OP_AREF) {
      /* A B C          R(A) := R(B)[C] */
      int a = GETARG_A(i);
      int c = GETARG_C(i);
      mrb_value v = regs[GETARG_B(i)];

      if (v.tt != MRB_TT_ARRAY) {
        if (c == 0) {
          regs[GETARG_A(i)] = v;
        }
        else {
          SET_NIL_VALUE(regs[a]);
        }
      }
      else {
        regs[GETARG_A(i)] = mrb_ary_ref(mrb, v, c);
      }
      NEXT;
    }

    CASE(OP_ASET) {
      /* A B C          R(B)[C] := R(A) */
      mrb_ary_set(mrb, regs[GETARG_B(i)], GETARG_C(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_APOST) {
      /* A B C  *R(A),R(A+1)..R(A+C) := R(A) */
      int a = GETARG_A(i);
      mrb_value v = regs[a];
      int pre  = GETARG_B(i);
      int post = GETARG_C(i);

      if (v.tt != MRB_TT_ARRAY) {
        regs[a++] = mrb_ary_new_capa(mrb, 0);
        while (post--) {
          SET_NIL_VALUE(regs[a]);
          a++;
        }
      }
      else {
        struct RArray *ary = mrb_ary_ptr(v);
        int len = ary->len;
        int i;

        if (len > pre + post) {
          regs[a++] = mrb_ary_new_elts(mrb, len - pre - post, ary->buf+pre);
          while (post--) {
            regs[a++] = ary->buf[len-post-1];
          }
        }
        else {
          regs[a++] = mrb_ary_new_capa(mrb, 0);
          for (i=0; i+pre<len; i++) {
            regs[a+i] = ary->buf[pre+i];
          }
          while (i < post) {
            SET_NIL_VALUE(regs[a+i]);
            i++;
          }
        }
      }
      NEXT;
    }

    CASE(OP_STRING) {
      /* A Bx           R(A) := str_new(Lit(Bx)) */
      regs[GETARG_A(i)] = mrb_str_literal(mrb, pool[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_STRCAT) {
      /* A B    R(A).concat(R(B)) */
      mrb_str_concat(mrb, regs[GETARG_A(i)], regs[GETARG_B(i)]);
      NEXT;
    }

    CASE(OP_HASH) {
      /* A B C   R(A) := hash_new(R(B),R(B+1)..R(B+C)) */
      int b = GETARG_B(i);
      int c = GETARG_C(i);
      int lim = b+c*2;
      mrb_value hash = mrb_hash_new_capa(mrb, c);

      while (b < lim) {
        mrb_hash_set(mrb, hash, regs[b], regs[b+1]);
        b+=2;
      }
      regs[GETARG_A(i)] = hash;
      NEXT;
    }

    CASE(OP_LAMBDA) {
      /* A b c  R(A) := lambda(SEQ[b],c) (b:c = 14:2) */
      struct RProc *p;
      int c = GETARG_c(i);

      if (c & OP_L_CAPTURE) {
        p = mrb_closure_new(mrb, mrb->irep[irep->idx+GETARG_b(i)]);
      }
      else {
        p = mrb_proc_new(mrb, mrb->irep[irep->idx+GETARG_b(i)]);
      }
      if (c & OP_L_STRICT) p->flags |= MRB_PROC_STRICT;
      regs[GETARG_A(i)] = mrb_obj_value(p);
      NEXT;
    }

    CASE(OP_OCLASS) {
      /* A      R(A) := ::Object */
      regs[GETARG_A(i)] = mrb_obj_value(mrb->object_class);
      NEXT;
    }

    CASE(OP_CLASS) {
      /* A B    R(A) := newclass(R(A),Sym(B),R(A+1)) */
      struct RClass *c = 0;
      int a = GETARG_A(i);
      mrb_value base, super;
      mrb_sym id = syms[GETARG_B(i)];

      base = regs[a];
      super = regs[a+1];
      if (mrb_nil_p(base)) {
        base = mrb_obj_value(mrb->ci->target_class);
      }
      c = mrb_vm_define_class(mrb, base, super, id);
      regs[a] = mrb_obj_value(c);
      NEXT;
    }

    CASE(OP_MODULE) {
      /* A B            R(A) := newmodule(R(A),Sym(B)) */
      struct RClass *c = 0;
      int a = GETARG_A(i);
      mrb_value base;
      mrb_sym id = syms[GETARG_B(i)];

      base = regs[a];
      if (mrb_nil_p(base)) {
        base = mrb_obj_value(mrb->ci->target_class);
      }
      c = mrb_vm_define_module(mrb, base, id);
      regs[a] = mrb_obj_value(c);
      NEXT;
    }

    CASE(OP_EXEC) {
      /* A Bx   R(A) := blockexec(R(A),SEQ[Bx]) */
      int a = GETARG_A(i);
      mrb_callinfo *ci;
      mrb_value recv = regs[a];
      struct RProc *p;

      /* prepare stack */
      ci = cipush(mrb);
      ci->pc = pc + 1;
      ci->acc = a;
      ci->mid = 0;
      ci->stackidx = mrb->stack - mrb->stbase;
      ci->argc = 0;
      ci->target_class = mrb_class_ptr(regs[GETARG_A(i)]);

      p = mrb_proc_new(mrb, mrb->irep[irep->idx+GETARG_Bx(i)]);
      p->target_class = ci->target_class;
      ci->proc = p;

      if (MRB_PROC_CFUNC_P(p)) {
        mrb->stack[0] = p->body.func(mrb, recv);
        mrb->arena_idx = ai;
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        regs = mrb->stack = mrb->stbase + ci->stackidx;
        cipop(mrb);
        NEXT;
      }
      else {
        /* setup environment for calling method */
        irep = p->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        mrb->stack += a;
        stack_extend(mrb, irep->nregs, 1);
        regs = mrb->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_METHOD) {
      /* A B            R(A).newmethod(Sym(B),R(A+1)) */
      int a = GETARG_A(i);
      struct RClass *c = mrb_class_ptr(regs[a]);

      mrb_define_method_vm(mrb, c, syms[GETARG_B(i)], regs[a+1]);
      NEXT;
    }

    CASE(OP_SCLASS) {
      /* A B    R(A) := R(B).singleton_class */
      regs[GETARG_A(i)] = mrb_singleton_class(mrb, regs[GETARG_B(i)]);
      NEXT;
    }

    CASE(OP_TCLASS) {
      /* A B    R(A) := target_class */
      regs[GETARG_A(i)] = mrb_obj_value(mrb->ci->target_class);
      NEXT;
    }

    CASE(OP_RANGE) {
      /* A B C  R(A) := range_new(R(B),R(B+1),C) */
      int b = GETARG_B(i);
      regs[GETARG_A(i)] = mrb_range_new(mrb, regs[b], regs[b+1], GETARG_C(i));
      NEXT;
    }

    CASE(OP_DEBUG) {
      /* A      debug print R(A),R(B),R(C) */
      printf("OP_DEBUG %d %d %d\n", GETARG_A(i), GETARG_B(i), GETARG_C(i));
      NEXT;
    }

    CASE(OP_STOP) {
      /*        stop VM */
    L_STOP:
      mrb->jmp = prev_jmp;
      return mrb_nil_value();
    }

    CASE(OP_ERR) {
      /* Bx     raise RuntimeError with message Lit(Bx) */
      mrb_value msg = pool[GETARG_Bx(i)];
      mrb_value exc = mrb_exc_new3(mrb, mrb->eRuntimeError_class, msg);

      mrb->exc = mrb_object(exc);
      goto L_RAISE;
    }
  }
  END_DISPACTH;
}
