#include "../inc/emitter.h"

#include "../std/std.h"

#include "../inc/debug.h"
#include "../inc/type.h"
#include "../inc/ast.h"
#include "../inc/sym.h"
#include "../inc/architecture.h"
#include "../inc/operand.h"
#include "../inc/asm.h"
#include "../inc/asm-amd64.h"
#include "../inc/reg.h"

#include "../inc/emitter-value.h"
#include "../inc/emitter-decl.h"

#include "string.h"
#include "stdlib.h"

static void emitterModule (emitterCtx* ctx, const ast* Node);

static void emitterFnImpl (emitterCtx* ctx, const ast* Node);
static void emitterLine (emitterCtx* ctx, const ast* Node);

static void emitterReturn (emitterCtx* ctx, const ast* Node);

static void emitterBranch (emitterCtx* ctx, const ast* Node);
static void emitterLoop (emitterCtx* ctx, const ast* Node);
static void emitterIter (emitterCtx* ctx, const ast* Node);

static emitterCtx* emitterInit (const char* output, const architecture* arch) {
    emitterCtx* ctx = malloc(sizeof(emitterCtx));
    ctx->Asm = asmInit(output, arch);
    ctx->arch = arch;
    ctx->labelReturnTo = operandCreate(operandUndefined);
    ctx->labelBreakTo = operandCreate(operandUndefined);
    return ctx;
}

static void emitterEnd (emitterCtx* ctx) {
    asmEnd(ctx->Asm);
    free(ctx);
}

void emitter (const ast* Tree, const char* output, const architecture* arch) {
    emitterCtx* ctx = emitterInit(output, arch);
    asmFilePrologue(ctx->Asm);

    emitterModule(ctx, Tree);

    asmFileEpilogue(ctx->Asm);
    emitterEnd(ctx);
}

static void emitterModule (emitterCtx* ctx, const ast* Node) {
    debugEnter("Module");

    for (ast* Current = Node->firstChild;
         Current;
         Current = Current->nextSibling) {
        if (Current->tag == astUsing) {
            if (Current->r)
                emitterModule(ctx, Current->r);

        } else if (Current->tag == astFnImpl)
            emitterFnImpl(ctx, Current);

        else if (Current->tag == astDecl)
            emitterDecl(ctx, Current);

        else if (Current->tag == astEmpty)
            debugMsg("Empty");

        else
            debugErrorUnhandled("emitterModule", "AST tag", astTagGetStr(Current->tag));
    }

    debugLeave();
}

static int emitterScopeAssignOffsets (const architecture* arch, sym* Scope, int offset) {
    for (int n = 0; n < Scope->children.length; n++) {
        sym* Symbol = vectorGet(&Scope->children, n);

        if (Symbol->tag == symScope)
            offset = emitterScopeAssignOffsets(arch, Symbol, offset);

        else if (Symbol->tag == symId) {
            offset -= typeGetSize(arch, Symbol->dt);
            Symbol->offset = offset;
            reportSymbol(Symbol);

        } else {}
    }

    return offset;
}

int emitterFnAllocateStack (const architecture* arch, sym* fn) {
    /*Two words already on the stack:
      return ptr and saved base pointer*/
    int lastOffset = 2*arch->wordsize;

    /*Returning through temporary?*/
    if (typeGetSize(arch, typeGetReturn(fn->dt)) > arch->wordsize)
        lastOffset += arch->wordsize;

    /*Assign offsets to all the parameters*/
    for (int n = 0; n < fn->children.length; n++) {
        sym* param = vectorGet(&fn->children, n);

        if (param->tag != symParam)
            break;

        param->offset = lastOffset;
        lastOffset += typeGetSize(arch, param->dt);

        reportSymbol(param);
    }

    /*Allocate stack space for all the auto variables
      Stack grows down, so the amount is the negation of the last offset*/
    return -emitterScopeAssignOffsets(arch, fn, 0);
}

static void emitterFnImpl (emitterCtx* ctx, const ast* Node) {
    debugEnter("FnImpl");

    if (Node->symbol->label == 0)
        ctx->arch->symbolMangler(Node->symbol);

    int stacksize = emitterFnAllocateStack(ctx->arch, Node->symbol);

    /*Label to jump to from returns*/
    operand EndLabel = ctx->labelReturnTo = asmCreateLabel(ctx->Asm, labelReturn);

    asmComment(ctx->Asm, "");
    asmFnPrologue(ctx->Asm, Node->symbol->label, stacksize);

    emitterCode(ctx, Node->r);
    asmFnEpilogue(ctx->Asm, EndLabel);

    debugLeave();
}

void emitterCode (emitterCtx* ctx, const ast* Node) {
    asmEnter(ctx->Asm);

    for (ast* Current = Node->firstChild;
         Current;
         Current = Current->nextSibling) {
        emitterLine(ctx, Current);
    }

    asmLeave(ctx->Asm);
}

static void emitterLine (emitterCtx* ctx, const ast* Node) {
    debugEnter("Line");

    asmComment(ctx->Asm, "");

    if (Node->tag == astBranch)
        emitterBranch(ctx, Node);

    else if (Node->tag == astLoop)
        emitterLoop(ctx, Node);

    else if (Node->tag == astIter)
        emitterIter(ctx, Node);

    else if (Node->tag == astCode)
        emitterCode(ctx, Node);

    else if (Node->tag == astReturn)
        emitterReturn(ctx, Node);

    else if (Node->tag == astBreak)
        asmJump(ctx->Asm, ctx->labelBreakTo);

    else if (Node->tag == astContinue)
        asmJump(ctx->Asm, ctx->labelContinueTo);

    else if (Node->tag == astDecl)
        emitterDecl(ctx, Node);

    else if (astIsValueTag(Node->tag))
        emitterValue(ctx, Node, requestVoid);

    else if (Node->tag == astEmpty)
        debugMsg("Empty");

    else
        debugErrorUnhandled("emitterLine", "AST tag", astTagGetStr(Node->tag));

    debugLeave();
}

static void emitterReturn (emitterCtx* ctx, const ast* Node) {
    debugEnter("Return");

    /*Non void return?*/
    if (Node->r)
        emitterValue(ctx, Node->r, requestReturn);

    asmJump(ctx->Asm, ctx->labelReturnTo);

    debugLeave();
}

static void emitterBranch (emitterCtx* ctx, const ast* Node) {
    debugEnter("Branch");

    operand ElseLabel = asmCreateLabel(ctx->Asm, labelElse);
    operand EndLabel = asmCreateLabel(ctx->Asm, labelEndIf);

    /*Compute the condition, requesting it be placed in the flags*/
    asmBranch(ctx->Asm,
              emitterValue(ctx, Node->firstChild, requestFlags),
              ElseLabel);

    emitterCode(ctx, Node->l);

    if (Node->r) {
        asmComment(ctx->Asm, "");
        asmJump(ctx->Asm, EndLabel);
        asmLabel(ctx->Asm, ElseLabel);

        emitterCode(ctx, Node->r);

        asmLabel(ctx->Asm, EndLabel);

    } else
        asmLabel(ctx->Asm, ElseLabel);

    debugLeave();
}

static void emitterLoop (emitterCtx* ctx, const ast* Node) {
    debugEnter("Loop");

    /*The place to return to loop again (after confirming condition)*/
    operand LoopLabel = asmCreateLabel(ctx->Asm, labelWhile);

    operand OldBreakTo = ctx->labelBreakTo;
    operand OldContinueTo = ctx->labelContinueTo;
    operand EndLabel = ctx->labelBreakTo = asmCreateLabel(ctx->Asm, labelBreak);
    ctx->labelContinueTo = asmCreateLabel(ctx->Asm, labelContinue);

    /*Work out which order the condition and code came in
      => whether this is a while or a do while*/
    bool isDo = Node->l->tag == astCode;
    ast* cond = isDo ? Node->r : Node->l;
    ast* code = isDo ? Node->l : Node->r;

    /*Condition*/

    if (!isDo)
        asmBranch(ctx->Asm,
                  emitterValue(ctx, cond, requestFlags),
                  EndLabel);

    /*Code*/

    asmLabel(ctx->Asm, LoopLabel);
    emitterCode(ctx, code);

    asmComment(ctx->Asm, "");

    /*Condition*/

    asmLabel(ctx->Asm, ctx->labelContinueTo);

    asmBranch(ctx->Asm,
              emitterValue(ctx, cond, requestFlags),
              EndLabel);

    asmJump(ctx->Asm, LoopLabel);
    asmLabel(ctx->Asm, EndLabel);

    ctx->labelBreakTo = OldBreakTo;
    ctx->labelContinueTo = OldContinueTo;

    debugLeave();
}

static void emitterIter (emitterCtx* ctx, const ast* Node) {
    debugEnter("Iter");

    ast* init = Node->firstChild;
    ast* cond = init->nextSibling;
    ast* iter = cond->nextSibling;

    operand LoopLabel = asmCreateLabel(ctx->Asm, labelFor);

    operand OldBreakTo = ctx->labelBreakTo;
    operand OldContinueTo = ctx->labelContinueTo;
    operand EndLabel = ctx->labelBreakTo = asmCreateLabel(ctx->Asm, labelBreak);
    ctx->labelContinueTo = asmCreateLabel(ctx->Asm, labelContinue);

    /*Initialize stuff*/

    if (init->tag == astDecl) {
        emitterDecl(ctx, init);
        asmComment(ctx->Asm, "");

    } else if (astIsValueTag(init->tag)) {
        emitterValue(ctx, init, requestVoid);
        asmComment(ctx->Asm, "");

    } else if (init->tag != astEmpty)
        debugErrorUnhandled("emitterIter", "AST tag", astTagGetStr(init->tag));


    /*Check condition*/

    asmLabel(ctx->Asm, LoopLabel);

    if (cond->tag != astEmpty) {
        operand Condition = emitterValue(ctx, cond, requestFlags);
        asmBranch(ctx->Asm, Condition, EndLabel);
    }

    /*Do block*/

    emitterCode(ctx, Node->l);
    asmComment(ctx->Asm, "");

    /*Iterate*/

    asmLabel(ctx->Asm, ctx->labelContinueTo);

    if (iter->tag != astEmpty) {
        emitterValue(ctx, iter, requestVoid);
        asmComment(ctx->Asm, "");
    }

    /*loopen Sie*/

    asmJump(ctx->Asm, LoopLabel);
    asmLabel(ctx->Asm, EndLabel);

    ctx->labelBreakTo = OldBreakTo;
    ctx->labelContinueTo = OldContinueTo;

    debugLeave();
}
