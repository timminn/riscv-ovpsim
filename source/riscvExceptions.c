/*
 * Copyright (c) 2005-2020 Imperas Software Ltd., www.imperas.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied.
 *
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

// Standard header files
#include <stdio.h>
#include <string.h>

// Imperas header files
#include "hostapi/impAlloc.h"

// VMI header files
#include "vmi/vmiMessage.h"
#include "vmi/vmiRt.h"

// model header files
#include "riscvCSR.h"
#include "riscvDecode.h"
#include "riscvExceptions.h"
#include "riscvExceptionDefinitions.h"
#include "riscvFunctions.h"
#include "riscvMessage.h"
#include "riscvStructure.h"
#include "riscvUtils.h"
#include "riscvVM.h"
#include "riscvVMConstants.h"


////////////////////////////////////////////////////////////////////////////////
// EXCEPTION DEFINITIONS
////////////////////////////////////////////////////////////////////////////////

//
// Fill one member of exceptions
//
#define RISCV_EXCEPTION(_NAME, _ARCH, _DESC) { \
    vmiInfo : {name:#_NAME, code:riscv_E_##_NAME, description:_DESC},    \
    arch    : _ARCH                                                      \
}

//
// Table of exception descriptors
//
static const riscvExceptionDesc exceptions[] = {

    ////////////////////////////////////////////////////////////////////
    // EXCEPTIONS
    ////////////////////////////////////////////////////////////////////

    RISCV_EXCEPTION (InstructionAddressMisaligned, 0,     "Fetch from unaligned address"),
    RISCV_EXCEPTION (InstructionAccessFault,       0,     "No access permission for fetch"),
    RISCV_EXCEPTION (IllegalInstruction,           0,     "Undecoded, unimplemented or disabled instruction"),
    RISCV_EXCEPTION (Breakpoint,                   0,     "EBREAK instruction executed"),
    RISCV_EXCEPTION (LoadAddressMisaligned,        0,     "Load from unaligned address"),
    RISCV_EXCEPTION (LoadAccessFault,              0,     "No access permission for load"),
    RISCV_EXCEPTION (StoreAMOAddressMisaligned,    0,     "Store/atomic memory operation at unaligned address"),
    RISCV_EXCEPTION (StoreAMOAccessFault,          0,     "No access permission for store/atomic memory operation"),
    RISCV_EXCEPTION (EnvironmentCallFromUMode,     ISA_U, "ECALL instruction executed in User mode"),
    RISCV_EXCEPTION (EnvironmentCallFromSMode,     ISA_S, "ECALL instruction executed in Supervisor mode"),
    RISCV_EXCEPTION (EnvironmentCallFromMMode,     0,     "ECALL instruction executed in Machine mode"),
    RISCV_EXCEPTION (InstructionPageFault,         0,     "Page fault at fetch address"),
    RISCV_EXCEPTION (LoadPageFault,                0,     "Page fault at load address"),
    RISCV_EXCEPTION (StoreAMOPageFault,            0,     "Page fault at store/atomic memory operation address"),

    ////////////////////////////////////////////////////////////////////
    // STANDARD INTERRUPTS
    ////////////////////////////////////////////////////////////////////

    RISCV_EXCEPTION (USWInterrupt,                 ISA_N, "User software interrupt"),
    RISCV_EXCEPTION (SSWInterrupt,                 ISA_S, "Supervisor software interrupt"),
    RISCV_EXCEPTION (MSWInterrupt,                 0,     "Machine software interrupt"),
    RISCV_EXCEPTION (UTimerInterrupt,              ISA_N, "User timer interrupt"),
    RISCV_EXCEPTION (STimerInterrupt,              ISA_S, "Supervisor timer interrupt"),
    RISCV_EXCEPTION (MTimerInterrupt,              0,     "Machine timer interrupt"),
    RISCV_EXCEPTION (UExternalInterrupt,           ISA_N, "User external interrupt"),
    RISCV_EXCEPTION (SExternalInterrupt,           ISA_S, "Supervisor external interrupt"),
    RISCV_EXCEPTION (MExternalInterrupt,           0,     "Machine external interrupt"),

    ////////////////////////////////////////////////////////////////////
    // TERMINATOR
    ////////////////////////////////////////////////////////////////////

    {{0}}
};


////////////////////////////////////////////////////////////////////////////////
// UTILITIES
////////////////////////////////////////////////////////////////////////////////

//
// Return current PC
//
inline static Uns64 getPC(riscvP riscv) {
    return vmirtGetPC((vmiProcessorP)riscv);
}

//
// Set current PC
//
inline static void setPCxRET(riscvP riscv, Uns64 newPC) {

    // mask exception return address to 32 bits if compressed instructions
    // are not currently enabled
    if(!(riscv->currentArch & ISA_C)) {
        newPC &= -4;
    }

    vmirtSetPC((vmiProcessorP)riscv, newPC);
}

//
// Clear any active exclusive access
//
inline static void clearEA(riscvP riscv) {
    riscv->exclusiveTag = RISCV_NO_TAG;
}

//
// Clear any active exclusive access on an xRET, if required
//
inline static void clearEAxRET(riscvP riscv) {
    if(!riscv->configInfo.xret_preserves_lr) {
        clearEA(riscv);
    }
}

//
// Return a Boolean indicating whether an active first-only-fault exception has
// been encountered, in which case no exception should be taken
//
static Bool handleFF(riscvP riscv) {

    Bool suppress = False;

    // is first-only-fault mode active?
    if(riscv->vFirstFault) {

        // deactivate first-only-fault mode (whether or not exception is to be
        // taken)
        riscv->vFirstFault = False;

        // special action required only if not the first element
        if(RD_CSR(riscv, vstart)) {

            // suppress the exception
            suppress = True;

            // clamp vl to current vstart
            riscvSetVL(riscv, RD_CSR(riscv, vstart));

            // set matching polymorphic key and clamped vl
            riscvRefreshVectorPMKey(riscv);
        }
    }

    return suppress;
}

//
// Halt the passed processor
//
static void haltProcessor(riscvP riscv, riscvDisableReason reason) {

    if(!riscv->disable) {
        vmirtHalt((vmiProcessorP)riscv);
    }

    riscv->disable |= reason;
}

//
// Restart the passed processor
//
static void restartProcessor(riscvP riscv, riscvDisableReason reason) {

    riscv->disable &= ~reason;

    // restart if no longer disabled (maybe from blocked state not visible in
    // disable code)
    if(!riscv->disable) {
        vmirtRestartNext((vmiProcessorP)riscv);
    }
}


////////////////////////////////////////////////////////////////////////////////
// TAKING EXCEPTIONS
////////////////////////////////////////////////////////////////////////////////

//
// Forward reference
//
static void enterDM(riscvP riscv, dmCause cause);

//
// Return PC to which to return after taking an exception. For processors with
// instruction table extensions, the address should be the original instruction,
// not the table instruction.
//
static Uns64 getEPC(riscvP riscv) {

    Uns8  dsOffset;
    Uns64 eretPC = vmirtGetPCDS((vmiProcessorP)riscv, &dsOffset);

    return dsOffset ? riscv->jumpBase : eretPC;
}

//
// Return the mode to which to take the given exception or interrupt (mode X)
//
static riscvMode getModeX(
    riscvP         riscv,
    Uns32          mMask,
    Uns32          sMask,
    riscvException ecode
) {
    riscvMode modeY = getCurrentMode(riscv);
    riscvMode modeX;

    // get mode X implied by delegation registers
    if(!(mMask & (1<<ecode))) {
        modeX = RISCV_MODE_MACHINE;
    } else if(!(sMask & (1<<ecode))) {
        modeX = RISCV_MODE_SUPERVISOR;
    } else {
        modeX = RISCV_MODE_USER;
    }

    // exception cannot be taken to lower-privilege mode
    return (modeX>modeY) ? modeX : modeY;
}

//
// Return the mode to which to take the given interrupt (mode X)
//
static riscvMode getInterruptModeX(riscvP riscv, riscvException ecode) {
    return getModeX(riscv, RD_CSR(riscv, mideleg), RD_CSR(riscv, sideleg), ecode);
}

//
// Return the mode to which to take the given exception (mode X)
//
static riscvMode getExceptionModeX(riscvP riscv, riscvException ecode) {
    return getModeX(riscv, RD_CSR(riscv, medeleg), RD_CSR(riscv, sedeleg), ecode);
}

//
// Is exception an interrupt?
//
#define IS_INTERRUPT(_EXCEPTION) ((_EXCEPTION) & riscv_E_Interrupt)

//
// Get code from exception
//
#define GET_ECODE(_EXCEPTION) ((_EXCEPTION) & ~riscv_E_Interrupt)

//
// Return interrupt mode (0:direct, 1:vectored) - from privileged ISA version
// 1.10 this is encoded in the [msu]tvec register, but previous versions did
// not support vectored mode except in some custom manner (for example, Andes
// N25 and NX25 processors)
//
inline static riscvICMode getIMode(riscvICMode customMode, riscvICMode tvecMode) {
    return tvecMode ? tvecMode : customMode;
}

//
// Update exception state when taking exception to mode X from mode Y
//
#define TARGET_MODE_X(_P, _X, _x, _IS_INT, _ECODE, _EPC, _BASE, _MODE, _TVAL) { \
                                                                                \
    /* get interrupt enable bit for mode X */                                   \
    Uns8 _IE = RD_CSR_FIELD(riscv, mstatus, _X##IE);                            \
                                                                                \
    /* update interrupt enable and interrupt enable stack */                    \
    WR_CSR_FIELD(riscv, mstatus, _X##PIE, _IE);                                 \
    WR_CSR_FIELD(riscv, mstatus, _X##IE, 0);                                    \
                                                                                \
    /* update cause register */                                                 \
    WR_CSR_FIELD(riscv, _x##cause,  ExceptionCode, _ECODE);                     \
    WR_CSR_FIELD(riscv, _x##cause,  Interrupt,     _IS_INT);                    \
                                                                                \
    /* update writable bits in epc register */                                  \
    Uns64 epcMask = RD_CSR_MASK(riscv, _x##epc);                                \
    WR_CSR_FIELD(riscv, _x##epc, value, (_EPC) & epcMask);                      \
                                                                                \
    /* update tval register */                                                  \
    WR_CSR_FIELD(riscv, _x##tval, value, _TVAL);                                \
                                                                                \
    /* get exception base address and mode */                                   \
    _BASE = (Addr)RD_CSR_FIELD(riscv, _x##tvec, BASE) << 2;                     \
    _MODE = getIMode(riscv->_X##IMode, RD_CSR_FIELD(riscv, _x##tvec, MODE));    \
}

//
// Does this exception code correspond to a retired instruction?
//
static Bool retiredCode(riscvException exception) {

    switch(exception) {

        case riscv_E_Breakpoint:
        case riscv_E_EnvironmentCallFromUMode:
        case riscv_E_EnvironmentCallFromSMode:
        case riscv_E_EnvironmentCallFromHMode:
        case riscv_E_EnvironmentCallFromMMode:
            return True;

        default:
            return False;
    }
}

//
// Does this exception code correspond to an Access Fault?
//
static Bool accessFaultCode(riscvException exception) {

    switch(exception) {

        case riscv_E_InstructionAccessFault:
        case riscv_E_LoadAccessFault:
        case riscv_E_StoreAMOAccessFault:
            return True;

        default:
            return False;
    }
}

//
// Notify a derived model of trap entry or exception return if required
//
inline static void notifyTrapDerived(
    riscvP              riscv,
    riscvMode           mode,
    riscvTrapNotifierFn notifier,
    void               *clientData
) {
    if(notifier) {
        notifier(riscv, mode, clientData);
    }
}

//
// Notify a derived model of exception return if required
//
inline static void notifyERETDerived(riscvP riscv, riscvMode mode) {

    riscvExtCBP extCB;

    // call derived model preMorph functions if required
    for(extCB=riscv->extCBs; extCB; extCB=extCB->next) {
        notifyTrapDerived(riscv, mode, extCB->ERETNotifier, extCB->clientData);
    }
}

//
// Is the exception an external
//
inline static Bool isExternalInterrupt(riscvException exception) {
    return (
        (exception>=riscv_E_UExternalInterrupt) &&
        (exception<=riscv_E_MExternalInterrupt)
    );
}

//
// Take processor exception
//
void riscvTakeException(
    riscvP         riscv,
    riscvException exception,
    Uns64          tval
) {
    if(inDebugMode(riscv)) {

        // terminate execution of program buffer
        vmirtAbortRepeat((vmiProcessorP)riscv);
        enterDM(riscv, DMC_NONE);

    } else {

        Bool        isInt     = IS_INTERRUPT(exception);
        Uns32       ecode     = GET_ECODE(exception);
        Uns32       ecodeMod  = ecode;
        Uns64       EPC       = getEPC(riscv);
        Uns64       handlerPC = 0;
        riscvMode   modeY     = getCurrentMode(riscv);
        riscvMode   modeX;
        riscvExtCBP extCB;
        Uns64       base;
        riscvICMode mode;

        // adjust baseInstructions based on the exception code to take into
        // account whether the previous instruction has retired, unless
        // inhibited by mcountinhibit.IR
        if(!retiredCode(exception) && !riscvInhibitInstret(riscv)) {
            riscv->baseInstructions++;
        }

        // latch or clear Access Fault detail depending on exception type
        if(accessFaultCode(exception)) {
            riscv->AFErrorOut = riscv->AFErrorIn;
        } else {
            riscv->AFErrorOut = riscv_AFault_None;
        }

        // clear any active exclusive access
        clearEA(riscv);

        // get exception target mode (X)
        if(isInt) {
            modeX = getInterruptModeX(riscv, ecode);
        } else {
            modeX = getExceptionModeX(riscv, ecode);
        }

        // modify code reported for external interrupts if required
        if(isExternalInterrupt(exception)) {
            Uns32 offset = exception-riscv_E_ExternalInterrupt;
            ecodeMod = riscv->extInt[offset] ? : ecode;
        }

        // update state dependent on target exception level
        if(modeX==RISCV_MODE_USER) {

            // target user mode
            TARGET_MODE_X(riscv, U, u, isInt, ecodeMod, EPC, base, mode, tval);

        } else if(modeX==RISCV_MODE_SUPERVISOR) {

            // target supervisor mode
            TARGET_MODE_X(riscv, S, s, isInt, ecodeMod, EPC, base, mode, tval);
            WR_CSR_FIELD(riscv, mstatus, SPP, modeY);

        } else {

            // target machine mode
            TARGET_MODE_X(riscv, M, m, isInt, ecodeMod, EPC, base, mode, tval);
            WR_CSR_FIELD(riscv, mstatus, MPP, modeY);
        }

        // handle direct or vectored exception
        if((mode == riscv_int_Direct) || !isInt) {
            handlerPC = base;
        } else {
            handlerPC = base + (4 * ecode);
        }

        // switch to target mode
        riscvSetMode(riscv, modeX);

        // indicate the taken exception
        riscv->exception = exception;

        // set address at which to execute
        vmirtSetPCException((vmiProcessorP)riscv, handlerPC);

        // notify derived model of exception entry if required
        for(extCB=riscv->extCBs; extCB; extCB=extCB->next) {
            notifyTrapDerived(riscv, modeX, extCB->trapNotifier, extCB->clientData);
        }
    }
}

//
// Return description of the given exception
//
static const char *getExceptionDesc(riscvException exception, char *buffer) {

    const char *result = 0;

    if(exception>=riscv_E_LocalInterrupt) {

        // indexed local interrupt
        sprintf(buffer, "Local interrupt %u", exception-riscv_E_LocalInterrupt);
        result = buffer;

    } else {

        // standard interrupt
        riscvExceptionDescCP desc;

        for(desc=&exceptions[0]; desc->vmiInfo.description && !result; desc++) {
            if(desc->vmiInfo.code==exception) {
                result = desc->vmiInfo.description;
            }
        }
    }

    return result;
}

//
// Report memory exception in verbose mode
//
static void reportMemoryException(
    riscvP         riscv,
    riscvException exception,
    Uns64          tval
) {
    if(riscv->verbose) {
        char buffer[32];
        vmiMessage("W", CPU_PREFIX "_IMA",
            SRCREF_FMT "%s (0x"FMT_Ax")",
            SRCREF_ARGS(riscv, getPC(riscv)),
            getExceptionDesc(exception, buffer), tval
        );
    }
}

//
// Take processor exception because of memory access error which could be
// suppressed for a fault-only-first instruction
//
void riscvTakeMemoryException(
    riscvP         riscv,
    riscvException exception,
    Uns64          tval
) {
    // force vstart to zero if required
    MASK_CSR(riscv, vstart);

    // take exception unless fault-only-first mode overrides it
    if(!handleFF(riscv)) {
        reportMemoryException(riscv, exception, tval);
        riscvTakeException(riscv, exception, tval);
    }
}

//
// Take Illegal Instruction exception
//
void riscvIllegalInstruction(riscvP riscv) {

    Uns64 tval = 0;

    // tval is either 0 or the instruction pattern
    if(riscv->configInfo.tval_ii_code) {
        tval = riscvGetInstruction(riscv, getPC(riscv));
    }

    riscvTakeException(riscv, riscv_E_IllegalInstruction, tval);
}

//
// Take Instruction Address Misaligned exception
//
void riscvInstructionAddressMisaligned(riscvP riscv, Uns64 tval) {

    riscvException exception = riscv_E_InstructionAddressMisaligned;

    reportMemoryException(riscv, exception, tval);
    riscvTakeException(riscv, exception, tval & -2);
}

//
// Take ECALL exception
//
void riscvECALL(riscvP riscv) {

    riscvMode      mode      = getCurrentMode(riscv);
    riscvException exception = riscv_E_EnvironmentCallFromUMode + mode;

    riscvTakeException(riscv, exception, 0);
}


////////////////////////////////////////////////////////////////////////////////
// EXCEPTION RETURN
////////////////////////////////////////////////////////////////////////////////

//
// Given a mode to which the processor is attempting to return, check that the
// mode is implemented on this processor and return the minimum implemented
// mode if not
//
static riscvMode getERETMode(riscvP riscv, riscvMode newMode, riscvMode minMode) {
    return riscvHasMode(riscv, newMode) ? newMode : minMode;
}

//
// From version 1.12, MRET and SRET clear MPRV when leaving M-mode if new mode
// is less privileged than M-mode
//
static void clearMPRV(riscvP riscv, riscvMode newMode) {
    if(
        (RISCV_PRIV_VERSION(riscv)>RVPV_20190405) &&
        (newMode!=RISCV_MODE_MACHINE)
    ) {
        WR_CSR_FIELD(riscv, mstatus, MPRV, 0);
    }
}

//
// Do common actions when returning from an exception
//
static void doERETCommon(
    riscvP    riscv,
    riscvMode retMode,
    riscvMode newMode,
    Uns64     epc
) {
    // switch to target mode
    riscvSetMode(riscv, newMode);

    // jump to return address
    setPCxRET(riscv, epc);

    // notify derived model of exception return if required
    notifyERETDerived(riscv, retMode);

    // check for pending interrupts
    riscvTestInterrupt(riscv);
}

//
// Return from M-mode exception
//
void riscvMRET(riscvP riscv) {

    // undefined behavior in Debug mode - NOP in this model
    if(!inDebugMode(riscv)) {

        Uns32     MPP     = RD_CSR_FIELD(riscv, mstatus, MPP);
        riscvMode minMode = riscvGetMinMode(riscv);
        riscvMode newMode = getERETMode(riscv, MPP, minMode);
        riscvMode retMode = RISCV_MODE_MACHINE;

        // clear any active exclusive access
        clearEAxRET(riscv);

        // restore previous MIE
        WR_CSR_FIELD(riscv, mstatus, MIE, RD_CSR_FIELD(riscv, mstatus, MPIE))

        // MPIE=1
        WR_CSR_FIELD(riscv, mstatus, MPIE, 1);

        // MPP=<minimum_supported_mode>
        WR_CSR_FIELD(riscv, mstatus, MPP, minMode);

        // clear mstatus.MPRV if required
        clearMPRV(riscv, newMode);

        // do common return actions
        doERETCommon(riscv, retMode, newMode, RD_CSR_FIELD(riscv, mepc, value));
    }
}

//
// Return from S-mode exception
//
void riscvSRET(riscvP riscv) {

    // undefined behavior in Debug mode - NOP in this model
    if(!inDebugMode(riscv)) {

        Uns32     SPP     = RD_CSR_FIELD(riscv, mstatus, SPP);
        riscvMode minMode = riscvGetMinMode(riscv);
        riscvMode newMode = getERETMode(riscv, SPP, minMode);
        riscvMode retMode = RISCV_MODE_SUPERVISOR;

        // clear any active exclusive access
        clearEAxRET(riscv);

        // restore previous SIE
        WR_CSR_FIELD(riscv, mstatus, SIE, RD_CSR_FIELD(riscv, mstatus, SPIE))

        // SPIE=1
        WR_CSR_FIELD(riscv, mstatus, SPIE, 1);

        // SPP=<minimum_supported_mode>
        WR_CSR_FIELD(riscv, mstatus, SPP, minMode);

        // clear mstatus.MPRV if required
        clearMPRV(riscv, newMode);

        // do common return actions
        doERETCommon(riscv, retMode, newMode, RD_CSR_FIELD(riscv, sepc, value));
    }
}

//
// Return from U-mode exception
//
void riscvURET(riscvP riscv) {

    // undefined behavior in Debug mode - NOP in this model
    if(!inDebugMode(riscv)) {

        riscvMode newMode = RISCV_MODE_USER;
        riscvMode retMode = RISCV_MODE_USER;

        // clear any active exclusive access
        clearEAxRET(riscv);

        // restore previous UIE
        WR_CSR_FIELD(riscv, mstatus, UIE, RD_CSR_FIELD(riscv, mstatus, UPIE))

        // UPIE=1
        WR_CSR_FIELD(riscv, mstatus, UPIE, 1);

        // do common return actions
        doERETCommon(riscv, retMode, newMode, RD_CSR_FIELD(riscv, uepc, value));
    }
}


////////////////////////////////////////////////////////////////////////////////
// DEBUG MODE
////////////////////////////////////////////////////////////////////////////////

//
// Update processor Debug mode stalled state
//
inline static void updateDMStall(riscvP riscv, Bool DMStall) {

    riscv->DMStall = DMStall;

    // halt or restart processor if required
    if(riscv->configInfo.debug_mode==RVDM_INTERRUPT) {
        // no action
    } else if(DMStall) {
        haltProcessor(riscv, RVD_DEBUG);
    } else {
        restartProcessor(riscv, RVD_DEBUG);
    }
}

//
// Update processor Debug mode state
//
inline static void setDM(riscvP riscv, Bool DM) {

    riscv->DM = DM;

    // indicate new Debug mode
    vmirtWriteNetPort((vmiProcessorP)riscv, riscv->DMPortHandle, DM);
}

//
// Enter Debug mode
//
static void enterDM(riscvP riscv, dmCause cause) {

    if(!inDebugMode(riscv)) {

        riscvCountState state;

        // get state before possible inhibit update
        riscvPreInhibit(riscv, &state);

        // update current state
        setDM(riscv, True);

        // save current mode
        WR_CSR_FIELD(riscv, dcsr, prv, getCurrentMode(riscv));

        // save cause
        WR_CSR_FIELD(riscv, dcsr, cause, cause);

        // save current instruction address
        WR_CSR(riscv, dpc, getEPC(riscv));

        // switch to Machine mode
        riscvSetMode(riscv, RISCV_MODE_MACHINE);

        // refresh state after possible inhibit update
        riscvPostInhibit(riscv, &state, False);
    }

    // halt or restart processor if required
    updateDMStall(riscv, True);

    // interrupt the processor if required
    if(riscv->configInfo.debug_mode==RVDM_INTERRUPT) {
        vmirtInterrupt((vmiProcessorP)riscv);
    }
}

//
// Leave Debug mode
//
static void leaveDM(riscvP riscv) {

    riscvMode       newMode = RD_CSR_FIELD(riscv, dcsr, prv);
    riscvMode       retMode = RISCV_MODE_MACHINE;
    riscvCountState state;

    // get state before possible inhibit update
    riscvPreInhibit(riscv, &state);

    // update current state
    setDM(riscv, False);

    // clear mstatus.MPRV if required
    clearMPRV(riscv, newMode);

    // do common return actions
    doERETCommon(riscv, retMode, newMode, RD_CSR_FIELD(riscv, dpc, value));

    // refresh state after possible inhibit update
    riscvPostInhibit(riscv, &state, False);

    // halt or restart processor if required
    updateDMStall(riscv, False);
}

//
// Enter or leave Debug mode
//
void riscvSetDM(riscvP riscv, Bool DM) {

    Bool oldDM = inDebugMode(riscv);

    if((oldDM==DM) || riscv->inSaveRestore) {
        // no change in state or state restore
    } else if(DM) {
        enterDM(riscv, DMC_HALTREQ);
    } else {
        leaveDM(riscv);
    }
}

//
// Update debug mode stall indication
//
void riscvSetDMStall(riscvP riscv, Bool DMStall) {
    updateDMStall(riscv, DMStall);
}

//
// Instruction step breakpoint callback
//
static VMI_ICOUNT_FN(riscvStepExcept) {

    riscvP riscv = (riscvP)processor;

    if(!inDebugMode(riscv) && RD_CSR_FIELD(riscv, dcsr, step)) {
        enterDM(riscv, DMC_STEP);
    }
}

//
// Set step breakpoint if required
//
void riscvSetStepBreakpoint(riscvP riscv) {

    if(!inDebugMode(riscv) && RD_CSR_FIELD(riscv, dcsr, step)) {
        vmirtSetModelTimer(riscv->stepTimer, 1);
    }
}

//
// Return from Debug mode
//
void riscvDRET(riscvP riscv) {

    if(!inDebugMode(riscv)) {

        // report FS state
        if(riscv->verbose) {
            vmiMessage("W", CPU_PREFIX "_NDM",
                SRCREF_FMT "Illegal instruction - not debug mode",
                SRCREF_ARGS(riscv, getPC(riscv))
            );
        }

        // take Illegal Instruction exception
        riscvIllegalInstruction(riscv);

    } else {

        // leave Debug mode
        leaveDM(riscv);
    }
}

//
// Take EBREAK exception
//
void riscvEBREAK(riscvP riscv) {

    riscvMode mode  = getCurrentMode(riscv);
    Bool      useDM = False;

    // determine whether ebreak should cause debug module entry
    if(inDebugMode(riscv)) {
        useDM = True;
    } else if(mode==RISCV_MODE_USER) {
        useDM = RD_CSR_FIELD(riscv, dcsr, ebreaku);
    } else if(mode==RISCV_MODE_SUPERVISOR) {
        useDM = RD_CSR_FIELD(riscv, dcsr, ebreaks);
    } else if(mode==RISCV_MODE_MACHINE) {
        useDM = RD_CSR_FIELD(riscv, dcsr, ebreakm);
    }

    if(useDM) {

        // don't count the ebreak instruction if dcsr.stopcount is set
        if(RD_CSR_FIELD(riscv, dcsr, stopcount)) {
            if(!riscvInhibitCycle(riscv)) {
                riscv->baseCycles++;
            }
            if(!riscvInhibitInstret(riscv)) {
                riscv->baseInstructions++;
            }
        }

        // handle EBREAK as Debug module action
        enterDM(riscv, DMC_EBREAK);

    } else {

        // handle EBREAK as normal exception
        riscvTakeException(riscv, riscv_E_Breakpoint, getPC(riscv));
    }
}


////////////////////////////////////////////////////////////////////////////////
// VMI INTERFACE ROUTINES
////////////////////////////////////////////////////////////////////////////////

//
// Read privilege exception handler
//
VMI_RD_PRIV_EXCEPT_FN(riscvRdPrivExcept) {

    riscvP riscv = (riscvP)processor;

    if(!riscvVMMiss(riscv, domain, MEM_PRIV_R, address, bytes, attrs)) {
        *action = VMI_LOAD_STORE_CONTINUE;
    }
}

//
// Write privilege exception handler
//
VMI_WR_PRIV_EXCEPT_FN(riscvWrPrivExcept) {

    riscvP riscv = (riscvP)processor;

    if(!riscvVMMiss(riscv, domain, MEM_PRIV_W, address, bytes, attrs)) {
        *action = VMI_LOAD_STORE_CONTINUE;
    }
}

//
// Read alignment exception handler
//
VMI_RD_ALIGN_EXCEPT_FN(riscvRdAlignExcept) {

    riscvP riscv = (riscvP)processor;

    riscvTakeMemoryException(riscv, riscv_E_LoadAddressMisaligned, address);

    return 0;
}

//
// Write alignment exception handler
//
VMI_WR_ALIGN_EXCEPT_FN(riscvWrAlignExcept) {

    riscvP riscv = (riscvP)processor;

    riscvTakeMemoryException(riscv, riscv_E_StoreAMOAddressMisaligned, address);

    return 0;
}

//
// Read abort exception handler
//
VMI_RD_ABORT_EXCEPT_FN(riscvRdAbortExcept) {

    riscvP riscv = (riscvP)processor;

    if(riscv->PTWActive) {
        riscv->PTWBadAddr = True;
    } else {
        riscvTakeMemoryException(riscv, riscv_E_LoadAccessFault, address);
    }
}

//
// Write abort exception handler
//
VMI_WR_ABORT_EXCEPT_FN(riscvWrAbortExcept) {

    riscvP riscv = (riscvP)processor;

    if(riscv->PTWActive) {
        riscv->PTWBadAddr = True;
    } else {
        riscvTakeMemoryException(riscv, riscv_E_StoreAMOAccessFault, address);
    }
}

//
// Fetch addresses are always snapped to a 2-byte boundary, irrespective of
// whether compressed instructions are implemented (see comments associated
// with the JALR instruction in the RISC-V User-level ISA)
//
VMI_FETCH_SNAP_FN(riscvFetchSnap) {

    return thisPC & -2;
}

//
// Validate instruction fetch from the passed address
//
static Bool validateFetchAddressInt(
    riscvP     riscv,
    memDomainP domain,
    Uns64      thisPC,
    Bool       complete
) {
    vmiProcessorP  processor = (vmiProcessorP)riscv;
    memAccessAttrs attrs     = complete ? MEM_AA_TRUE : MEM_AA_FALSE;

    if(vmirtIsExecutable(processor, thisPC)) {

        // no exception pending
        return True;

    } else if(riscvVMMiss(riscv, domain, MEM_PRIV_X, thisPC, 2, attrs)) {

        // permission exception of some kind, handled by riscvVMMiss, so no
        // further action required here.
        return False;

    } else if(!vmirtIsExecutable(processor, thisPC)) {

        // bus error if address is not executable
        if(complete) {
            riscvTakeException(riscv, riscv_E_InstructionAccessFault, thisPC);
        }

        return False;

    } else {

        // no exception pending
        return True;
    }
}

//
// Validate that the passed address is a mapped fetch address (NOTE: address
// alignment is not validated here but by the preceding branch instruction)
//
static Bool validateFetchAddress(
    riscvP     riscv,
    memDomainP domain,
    Uns64      thisPC,
    Bool       complete
) {
    if(!validateFetchAddressInt(riscv, domain, thisPC, complete)) {

        // fetch exception (handled in validateFetchAddressInt)
        return False;

    } else if(riscvGetInstructionSize(riscv, thisPC) <= 2) {

        // instruction at simPC is a two-byte instruction
        return True;

    } else if(!validateFetchAddressInt(riscv, domain, thisPC+2, complete)) {

        // fetch exception (handled in validateFetchAddressInt)
        return False;

    } else {

        // no exception
        return True;
    }

    // no exception pending
    return True;
}

//
// Return interrupt enable for the passed mode, given a raw interrupt enable
// bit
//
inline static Bool getIE(riscvP riscv, Bool IE, riscvMode modeIE) {

    riscvMode mode = getCurrentMode(riscv);

    return (mode<modeIE) ? True : (mode>modeIE) ? False : IE;
}

//
// Return mask of pending interrupts that would cause resumption from WFI (note
// that these could however be masked by global interrupt bits or delegation
// bits - see the Privileged Architecture specification)
//
inline static Uns64 getPendingInterrupts(riscvP riscv) {
    return RD_CSR(riscv, mie) & RD_CSR(riscv, mip);
}

//
// Return mask of pending-and-enabled interrupts
//
static Uns64 getPendingAndEnabledInterrupts(riscvP riscv) {

    // NOTE: all interrupts are disabled in Debug mode
    Uns64 result = inDebugMode(riscv) ? 0 : getPendingInterrupts(riscv);

    if(result) {

        // get raw interrupt enable bits
        Bool MIE = RD_CSR_FIELD(riscv, mstatus, MIE);
        Bool SIE = RD_CSR_FIELD(riscv, mstatus, SIE);
        Bool UIE = RD_CSR_FIELD(riscv, mstatus, UIE);

        // modify effective interrupt enables based on current mode
        MIE = getIE(riscv, MIE, RISCV_MODE_MACHINE);
        SIE = getIE(riscv, SIE, RISCV_MODE_SUPERVISOR);
        UIE = getIE(riscv, UIE, RISCV_MODE_USER);

        // get interrupt mask applicable for each mode
        Uns64 mideleg = RD_CSR(riscv, mideleg);
        Uns64 sideleg = RD_CSR(riscv, sideleg) & mideleg;
        Uns64 mMask   = ~mideleg;
        Uns64 sMask   = mideleg & ~sideleg;
        Uns64 uMask   = sideleg;

        // handle masked interrupts
        if(!MIE) {result &= ~mMask;}
        if(!SIE) {result &= ~sMask;}
        if(!UIE) {result &= ~uMask;}
    }

    // return pending and enabled interrupts
    return result;
}

//
// Get priority for the indexed interrupt
//
static Uns32 getIntPri(Uns32 intNum) {

    #define INT_INDEX(_NAME) (riscv_E_##_NAME-riscv_E_Interrupt)

    // static table of priority mappings (NOTE: local and custom interrupts are
    // assumed to be lowest priority, indicated by default value 0 in this
    // table and value returned when out of range below)
    static const Uns8 intPri[INT_INDEX(Last)] = {
        [INT_INDEX(UTimerInterrupt)]    = 1,
        [INT_INDEX(USWInterrupt)]       = 2,
        [INT_INDEX(UExternalInterrupt)] = 3,
        [INT_INDEX(STimerInterrupt)]    = 4,
        [INT_INDEX(SSWInterrupt)]       = 5,
        [INT_INDEX(SExternalInterrupt)] = 6,
        [INT_INDEX(MTimerInterrupt)]    = 7,
        [INT_INDEX(MSWInterrupt)]       = 8,
        [INT_INDEX(MExternalInterrupt)] = 9,
    };

    return (intNum>=INT_INDEX(Last)) ? 0 : intPri[intNum];
}

//
// Descriptor for pending-and-enabled interrupt
//
typedef struct intDescS {
    Uns32     ecode;    // exception code
    riscvMode emode;    // mode to which taken
} intDesc;

//
// Process highest-priority interrupt in the given mask of pending-and-enabled
// interrupts
//
static void doInterrupt(riscvP riscv, Uns64 intMask) {

    Uns32   ecode    = 0;
    intDesc selected = {ecode:-1};

    // sanity check there are pending-and-enabled interrupts
    VMI_ASSERT(intMask, "expected pending-and-enabled interrupts");

    // find the highest priority pending-and-enabled interrupt
    do {

        if(intMask&1) {

            intDesc try = {ecode:ecode, emode:getInterruptModeX(riscv, ecode)};

            if(selected.ecode==-1) {
                // first pending-and-enabled interrupt
                selected = try;
            } else if(selected.emode < try.emode) {
                // higher destination privilege mode
                selected = try;
            } else if(selected.emode > try.emode) {
                // lower destination privilege mode
            } else if(getIntPri(selected.ecode)<=getIntPri(try.ecode)) {
                // higher fixed priority order and same destination mode
                selected = try;
            }
        }

        // step to next potential pending-and-enabled interrupt
        intMask >>= 1;
        ecode++;

    } while(intMask);

    // take the interrupt
    riscvTakeException(riscv, riscv_E_Interrupt+selected.ecode, 0);
}

//
// This is called by the simulator when fetching from an instruction address.
// It gives the model an opportunity to take an exception instead.
//
VMI_IFETCH_FN(riscvIFetchExcept) {

    riscvP riscv   = (riscvP)processor;
    Uns64  thisPC  = address;
    Bool   fetchOK = False;
    Uns64  intMask = getPendingAndEnabledInterrupts(riscv);

    if(riscv->netValue.resethaltreqS) {

        // enter Debug mode out of reset
        if(complete) {
            riscv->netValue.resethaltreqS = False;
            enterDM(riscv, DMC_RESETHALTREQ);
        }

    } else if(riscv->netValue.haltreq && !inDebugMode(riscv)) {

        // enter Debug mode
        if(complete) {
            enterDM(riscv, DMC_HALTREQ);
        }

    } else if(intMask) {

        // handle pending interrupt
        if(complete) {
            doInterrupt(riscv, intMask);
        }

    } else if(!validateFetchAddress(riscv, domain, thisPC, complete)) {

        // fetch exception (handled in validateFetchAddress)

    } else {

        // no exception pending
        fetchOK = True;
    }

    if(fetchOK) {
        return VMI_FETCH_NONE;
    } else if(complete) {
        return VMI_FETCH_EXCEPTION_COMPLETE;
    } else {
        return VMI_FETCH_EXCEPTION_PENDING;
    }
}

//
// Does the processor implement the exception or interrupt?
//
static Bool hasException(riscvP riscv, riscvException code) {

    if(code<riscv_E_Interrupt) {
        return riscv->exceptionMask & (1ULL<<code);
    } else {
        return riscv->interruptMask & (1ULL<<(code-riscv_E_Interrupt));
    }
}

//
// Return total number of interrupts (including 0 to 15)
//
inline static Uns32 getIntNum(riscvP riscv) {
    return riscv->configInfo.local_int_num+16;
}

//
// Return number of local interrupts
//
static Uns32 getLocalIntNum(riscvP riscv) {

    Bool isContainer = vmirtGetSMPChild((vmiProcessorP)riscv);

    return isContainer ? 0 : riscv->configInfo.local_int_num;
}

//
// Return all defined exceptions, including those from intercepts, in a null
// terminated list
//
static vmiExceptionInfoCP getExceptions(riscvP riscv) {

    if(!riscv->exceptions) {

        Uns32       numLocal = getLocalIntNum(riscv);
        Uns32       numExcept;
        riscvExtCBP extCB;
        Uns32       i;

        // get number of exceptions and standard interrupts in the base model
        for(i=0, numExcept=0; exceptions[i].vmiInfo.name; i++) {
            if(hasException(riscv, exceptions[i].vmiInfo.code)) {
                numExcept++;
            }
        }

        // include exceptions for derived model
        for(extCB=riscv->extCBs; extCB; extCB=extCB->next) {
            if(extCB->firstException) {
                vmiExceptionInfoCP list = extCB->firstException(
                    riscv, extCB->clientData
                );
                while(list && list->name) {
                    numExcept++; list++;
                }
            }
        }

        // count local exceptions
        numExcept += numLocal;

        // record total number of exceptions
        riscv->exceptionNum = numExcept;

        // allocate list of exceptions including null terminator
        vmiExceptionInfoP all = STYPE_CALLOC_N(vmiExceptionInfo, numExcept+1);

        // fill exceptions and standard interrupts from base model
        for(i=0, numExcept=0; exceptions[i].vmiInfo.name; i++) {
            if(hasException(riscv, exceptions[i].vmiInfo.code)) {
                all[numExcept++] = exceptions[i].vmiInfo;
            }
        }

        // fill exceptions from derived model
        for(extCB=riscv->extCBs; extCB; extCB=extCB->next) {
            if(extCB->firstException) {
                vmiExceptionInfoCP list = extCB->firstException(
                    riscv, extCB->clientData
                );
                while(list && list->name) {
                    all[numExcept++] = *list++;
                }
            }
        }

        // fill local exceptions
        for(i=0; i<numLocal; i++) {

            vmiExceptionInfoP this = &all[numExcept++];
            char              buffer[32];

            // construct name
            sprintf(buffer, "LocalInterrupt%u", i);

            this->code        = riscv_E_LocalInterrupt+i;
            this->name        = strdup(buffer);
            this->description = strdup(getExceptionDesc(this->code, buffer));
        }

        // save list on base model
        riscv->exceptions = all;
    }

    return riscv->exceptions;
}

//
// Get last-activated exception
//
VMI_GET_EXCEPTION_FN(riscvGetException) {

    riscvP             riscv     = (riscvP)processor;
    vmiExceptionInfoCP this      = getExceptions(riscv);
    riscvException     exception = riscv->exception;

    // get the first exception with matching code
    while(this->name && (this->code!=exception)) {
        this++;
    }

    return this->name ? this : 0;
}

//
// Iterate exceptions implemented on this variant
//
VMI_EXCEPTION_INFO_FN(riscvExceptionInfo) {

    riscvP             riscv = (riscvP)processor;
    vmiExceptionInfoCP this  = prev ? prev+1 : getExceptions(riscv);

    return this->name ? this : 0;
}

//
// Return mask of implemented local interrupts
//
Uns64 riscvGetLocalIntMask(riscvP riscv) {

    Uns32 localIntNum    = getLocalIntNum(riscv);
    Uns32 localShift     = (localIntNum<48) ? localIntNum : 48;
    Uns64 local_int_mask = (1ULL<<localShift)-1;

    return local_int_mask << riscv_E_Local;
}

//
// Initialize mask of implemented exceptions
//
void riscvSetExceptionMask(riscvP riscv) {

    riscvArchitecture    arch          = riscv->configInfo.arch;
    Uns64                exceptionMask = 0;
    Uns64                interruptMask = 0;
    riscvExceptionDescCP thisDesc;

    // get exceptions and standard interrupts supported on the current
    // architecture
    for(thisDesc=exceptions; thisDesc->vmiInfo.name; thisDesc++) {

        riscvException code = thisDesc->vmiInfo.code;

        if((arch&thisDesc->arch)!=thisDesc->arch) {
            // not implemented by this variant
        } else if(code<riscv_E_Interrupt) {
            exceptionMask |= 1ULL<<code;
        } else {
            interruptMask |= 1ULL<<(code-riscv_E_Interrupt);
        }
    }

    // save composed exception mask result
    riscv->exceptionMask = exceptionMask;

    // save composed interrupt mask result (including extra local interrupts
    // and excluding interrupts that are explicitly absent)
    riscv->interruptMask = (
        (interruptMask | riscvGetLocalIntMask(riscv)) &
        ~riscv->configInfo.unimp_int_mask
    );
}

//
// Free exception state
//
void riscvExceptFree(riscvP riscv) {

    if(riscv->exceptions) {

        Uns32              numLocal    = getLocalIntNum(riscv);
        Uns32              numNotLocal = riscv->exceptionNum - numLocal;
        vmiExceptionInfoCP local       = &riscv->exceptions[numNotLocal];
        Uns32              i;

        // free local exception description strings
        for(i=0; i<numLocal; i++) {
            free((char *)(local[i].name));
            free((char *)(local[i].description));
        }

        // free exception descriptions
        STYPE_FREE(riscv->exceptions);
        riscv->exceptions = 0;
    }
}


////////////////////////////////////////////////////////////////////////////////
// EXTERNAL INTERRUPT UTILITIES
////////////////////////////////////////////////////////////////////////////////

//
// Detect rising edge
//
inline static Bool posedge(Bool old, Bool new) {
    return !old && new;
}

//
// Detect falling edge
//
inline static Bool negedge(Uns32 old, Uns32 new) {
    return old && !new;
}

//
// Halt the processor in WFI state if required
//
void riscvWFI(riscvP riscv) {

    if(!(inDebugMode(riscv) || getPendingInterrupts(riscv))) {
        haltProcessor(riscv, RVD_WFI);
    }
}

//
// Check for pending interrupts
//
void riscvTestInterrupt(riscvP riscv) {

    Uns64 pendingEnabled = getPendingAndEnabledInterrupts(riscv);

    // print exception status
    if(RISCV_DEBUG_EXCEPT(riscv)) {

        // get factors contributing to interrupt state
        riscvIntState intState = {
            .pendingEnabled  = pendingEnabled,
            .pending         = RD_CSR(riscv, mip),
            .pendingExternal = riscv->ip[0],
            .pendingInternal = riscv->swip,
            .mideleg         = RD_CSR(riscv, mideleg),
            .sideleg         = RD_CSR(riscv, sideleg),
            .mie             = RD_CSR_FIELD(riscv, mstatus, MIE),
            .sie             = RD_CSR_FIELD(riscv, mstatus, SIE),
            .uie             = RD_CSR_FIELD(riscv, mstatus, UIE),
        };

        // report only if interrupt state changes
        if(memcmp(&riscv->intState, &intState, sizeof(intState))) {

            vmiMessage("I", CPU_PREFIX "_IS",
                SRCREF_FMT
                "PENDING+ENABLED="FMT_A08x" PENDING="FMT_A08x" "
                "[EXTERNAL_IP="FMT_A08x",SW_IP=%08x] "
                "MIDELEG=%08x SIDELEG=%08x MSTATUS.[MSU]IE=%u%u%u",
                SRCREF_ARGS(riscv, getPC(riscv)),
                intState.pendingEnabled,
                intState.pending,
                intState.pendingExternal,
                intState.pendingInternal,
                intState.mideleg,
                intState.sideleg,
                intState.mie,
                intState.sie,
                intState.uie
            );

            // track previous pending state
            riscv->intState = intState;
        }
    }

    // restart processor if it is halted in WFI state and local interrupts are
    // pending (even if masked)
    if(getPendingInterrupts(riscv)) {
        restartProcessor(riscv, RVD_RESTART_WFI);
    }

    // schedule asynchronous interrupt handling if interrupts are pending and
    // enabled
    if(pendingEnabled) {
        vmirtDoSynchronousInterrupt((vmiProcessorP)riscv);
    }
}

//
// Reset the processor
//
void riscvReset(riscvP riscv) {

    riscvExtCBP extCB;

    // restart the processor from any halted state
    restartProcessor(riscv, RVD_RESTART_RESET);

    // exit Debug mode
    riscvSetDM(riscv, False);

    // switch to Machine mode
    riscvSetMode(riscv, RISCV_MODE_MACHINE);

    // reset CSR state
    riscvCSRReset(riscv);

    // notify dependent model of reset event
    for(extCB=riscv->extCBs; extCB; extCB=extCB->next) {
        if(extCB->resetNotifier) {
            extCB->resetNotifier(riscv, extCB->clientData);
        }
    }

    // indicate the taken exception
    riscv->exception = 0;

    // set address at which to execute
    vmirtSetPCException((vmiProcessorP)riscv, riscv->configInfo.reset_address);

    // enter Debug mode out of reset if required
    riscv->netValue.resethaltreqS = riscv->netValue.resethaltreq;
}

//
// Do NMI interrupt
//
static void doNMI(riscvP riscv) {

    // restart the processor from any halted state
    restartProcessor(riscv, RVD_RESTART_NMI);

    // switch to Machine mode
    riscvSetMode(riscv, RISCV_MODE_MACHINE);

    // update cause register (to zero)
    WR_CSR(riscv, mcause, 0);

    // update mepc to hold next instruction address
    WR_CSR(riscv, mepc, getEPC(riscv));

    // indicate the taken exception
    riscv->exception = 0;

    // set address at which to execute
    vmirtSetPCException((vmiProcessorP)riscv, riscv->configInfo.nmi_address);
}


////////////////////////////////////////////////////////////////////////////////
// EXTERNAL INTERRUPT INTERFACE FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

//
// Update interrupt state because of some pending state change (either from
// external interrupt source or software pending register)
//
void riscvUpdatePending(riscvP riscv) {

    Uns64 oldValue = RD_CSR(riscv, mip);

    // compose new value from discrete sources
    Uns64 newValue = (riscv->ip[0] | riscv->swip);

    // update register value and exception state on a change
    if(oldValue != newValue) {
        WR_CSR(riscv, mip, newValue);
        riscvTestInterrupt(riscv);
    }
}

//
// Reset signal
//
static VMI_NET_CHANGE_FN(resetPortCB) {

    riscvInterruptInfoP ii       = userData;
    riscvP              riscv    = ii->hart;
    Bool                oldValue = riscv->netValue.reset;

    if(posedge(oldValue, newValue)) {

        // halt the processor while signal goes high
        haltProcessor(riscv, RVD_RESET);

    } else if(negedge(oldValue, newValue)) {

        // reset the processor when signal goes low
        riscvReset(riscv);
    }

    riscv->netValue.reset = newValue;
}

//
// NMI signal
//
static VMI_NET_CHANGE_FN(nmiPortCB) {

    riscvInterruptInfoP ii       = userData;
    riscvP              riscv    = ii->hart;
    Bool                oldValue = riscv->netValue.nmi;

    // do NMI actions when signal goes low unless in Debug mode
    if(!inDebugMode(riscv) && negedge(oldValue, newValue)) {
        doNMI(riscv);
    }

    // mirror value in dcsr.nmip
    WR_CSR_FIELD(riscv, dcsr, nmip, newValue);

    riscv->netValue.nmi = newValue;
}

//
// haltreq signal (edge triggered)
//
static VMI_NET_CHANGE_FN(haltreqPortCB) {

    riscvInterruptInfoP ii       = userData;
    riscvP              riscv    = ii->hart;
    Bool                oldValue = riscv->netValue.haltreq;

    // do halt actions when signal goes high unless in Debug mode
    if(!inDebugMode(riscv) && posedge(oldValue, newValue)) {
        vmirtDoSynchronousInterrupt((vmiProcessorP)riscv);
    }

    riscv->netValue.haltreq = newValue;
}

//
// resethaltreq signal (sampled at reset)
//
static VMI_NET_CHANGE_FN(resethaltreqPortCB) {

    riscvInterruptInfoP ii    = userData;
    riscvP              riscv = ii->hart;

    riscv->netValue.resethaltreq = newValue;
}

//
// Generic interrupt signal
//
static VMI_NET_CHANGE_FN(interruptPortCB) {

    riscvInterruptInfoP ii     = userData;
    riscvP              riscv  = ii->hart;
    Uns32               index  = ii->userData;
    Uns32               offset = index/64;
    Uns64               mask   = 1ULL << (index&63);
    Uns32               maxNum = getIntNum(riscv);

    // sanity check
    VMI_ASSERT(
        index<maxNum,
        "interrupt port index %u exceeds maximum %u",
        index, maxNum-1
    );

    if(newValue) {
        riscv->ip[offset] |= mask;
    } else {
        riscv->ip[offset] &= ~mask;
    }

    riscvUpdatePending(riscv);
}

//
// Generic interrupt ID signal
//
static VMI_NET_CHANGE_FN(interruptIDPortCB) {

    riscvInterruptInfoP ii     = userData;
    riscvP              riscv  = ii->hart;
    Uns32               offset = ii->userData;

    // sanity check
    VMI_ASSERT(
        offset<RISCV_MODE_LAST,
        "interrupt ID port index %u out of range",
        offset
    );

    riscv->extInt[offset] = newValue;
}


////////////////////////////////////////////////////////////////////////////////
// NET PORT CREATION
////////////////////////////////////////////////////////////////////////////////

//
// Convert bits to number of double words
//
#define BITS_TO_DWORDS(_B) (((_B)+63)/64)

//
// Allocate a new port and append to the tail of the list
//
static riscvNetPortPP newNetPort(
    riscvP         hart,
    riscvNetPortPP tail,
    const char    *name,
    vmiNetPortType type,
    vmiNetChangeFn portCB,
    const char    *desc,
    Uns32          code,
    Uns32         *handle
) {
    riscvNetPortP       this = STYPE_CALLOC(riscvNetPort);
    vmiNetPortP         info = &this->desc;
    riscvInterruptInfoP ii   = &this->ii;

    // fill port fields
    info->name        = strdup(name);
    info->type        = type;
    info->netChangeCB = portCB;
    info->handle      = handle;
    info->description = strdup(desc);
    info->userData    = ii;

    // initialize interrupt information structure to enable vectoring interrupt
    // to specific processor instance and use as userData on netChange callback
    ii->hart     = hart;
    ii->userData = code;

    // append to list
    *tail = this;

    // return new tail
    return &this->next;
}

//
// Allocate ports for this variant
//
void riscvNewNetPorts(riscvP riscv) {

    riscvNetPortPP tail = &riscv->netPorts;

    // allocate interrupt port state
    riscv->ipDWords = BITS_TO_DWORDS(getIntNum(riscv));
    riscv->ip       = STYPE_CALLOC_N(Uns64, riscv->ipDWords);

    // allocate reset port
    tail = newNetPort(
        riscv,
        tail,
        "reset",
        vmi_NP_INPUT,
        resetPortCB,
        "Reset",
        0,
        0
    );

    // allocate nmi port
    tail = newNetPort(
        riscv,
        tail,
        "nmi",
        vmi_NP_INPUT,
        nmiPortCB,
        "NMI",
        0,
        0
    );

    // allocate implemented interrupt ports
    riscvExceptionDescCP this;

    // get standard interrupts supported on the current architecture
    for(this=exceptions; this->vmiInfo.name; this++) {

        vmiExceptionInfoCP info = &this->vmiInfo;
        riscvException     code = info->code;

        if((code>=riscv_E_Interrupt) && hasException(riscv, code)) {

            tail = newNetPort(
                riscv,
                tail,
                info->name,
                vmi_NP_INPUT,
                interruptPortCB,
                info->description,
                code - riscv_E_Interrupt,
                0
            );

            if(!riscv->configInfo.external_int_id) {

                // no action unless External Interrupt code nets required

            } else if(!isExternalInterrupt(code)) {

                // no action unless this is an External Interrupt

            } else {

                // port names for each mode
                static const char *map[] = {
                    [RISCV_MODE_USER]       = "UExternalInterruptID",
                    [RISCV_MODE_SUPERVISOR] = "SExternalInterruptID",
                    [RISCV_MODE_HYPERVISOR] = "HExternalInterruptID",
                    [RISCV_MODE_MACHINE]    = "MExternalInterruptID",
                };

                Uns32 offset = code-riscv_E_ExternalInterrupt;

                tail = newNetPort(
                    riscv,
                    tail,
                    map[offset],
                    vmi_NP_INPUT,
                    interruptIDPortCB,
                    "External Interrupt ID",
                    offset,
                    0
                );
            }
        }
    }

    // add local interrupt ports
    Uns32 localIntNum = getLocalIntNum(riscv);
    Uns32 i;

    for(i=0; i<localIntNum; i++) {

        // synthesize code
        riscvException code = riscv_E_LocalInterrupt+i;

        // construct name and description
        char name[32];
        char desc[32];
        sprintf(name, "LocalInterrupt%u", i);
        sprintf(desc, "Local Interrupt %u", i);

        tail = newNetPort(
            riscv,
            tail,
            name,
            vmi_NP_INPUT,
            interruptPortCB,
            desc,
            code - riscv_E_Interrupt,
            0
        );
    }

    // add Debug mode ports
    if(riscv->configInfo.debug_mode) {

        // allocate DM port
        tail = newNetPort(
            riscv,
            tail,
            "DM",
            vmi_NP_OUTPUT,
            0,
            "Debug state indication",
            0,
            &riscv->DMPortHandle
        );

        // allocate haltreq port
        tail = newNetPort(
            riscv,
            tail,
            "haltreq",
            vmi_NP_INPUT,
            haltreqPortCB,
            "haltreq (Debug halt request)",
            0,
            0
        );

        // allocate resethaltreq port
        tail = newNetPort(
            riscv,
            tail,
            "resethaltreq",
            vmi_NP_INPUT,
            resethaltreqPortCB,
            "resethaltreq (Debug halt request after reset)",
            0,
            0
        );
    }
}

//
// Free ports
//
void riscvFreeNetPorts(riscvP riscv) {

    riscvNetPortP next = riscv->netPorts;
    riscvNetPortP this;

    // free interrupt port state
    STYPE_FREE(riscv->ip);

    // free ports
    while((this=next)) {

        next = this->next;

        // free name and description
        free((char *)(this->desc.name));
        free((char *)(this->desc.description));

        STYPE_FREE(this);
    }

    riscv->netPorts = 0;
}

//
// Get the next net port
//
VMI_NET_PORT_SPECS_FN(riscvNetPortSpecs) {

    riscvP        riscv = (riscvP)processor;
    riscvNetPortP this;

    if(!prev) {
        this = riscv->netPorts;
    } else {
        this = ((riscvNetPortP)prev)->next;
    }

    return this ? &this->desc : 0;
}


////////////////////////////////////////////////////////////////////////////////
// TIMER CREATION
////////////////////////////////////////////////////////////////////////////////

//
// Allocate timers
//
void riscvNewTimers(riscvP riscv) {

    if(riscv->configInfo.debug_mode) {
        riscv->stepTimer = vmirtCreateModelTimer(
            (vmiProcessorP)riscv, riscvStepExcept, 1, 0
        );
    }
}

//
// Free timers
//
void riscvFreeTimers(riscvP riscv) {

    if(riscv->stepTimer) {
        vmirtDeleteModelTimer(riscv->stepTimer);
    }
}


////////////////////////////////////////////////////////////////////////////////
// SAVE/RESTORE SUPPORT
////////////////////////////////////////////////////////////////////////////////

//
// Save net state not covered by register read/write API
//
void riscvNetSave(
    riscvP              riscv,
    vmiSaveContextP     cxt,
    vmiSaveRestorePhase phase
) {
    if(phase==SRT_END_CORE) {

        // save pending interrupt state
        vmirtSave(cxt, "ip", riscv->ip, riscv->ipDWords*8);

        // save latched control input state
        VMIRT_SAVE_FIELD(cxt, riscv, netValue);
        VMIRT_SAVE_FIELD(cxt, riscv, intState);
    }
}

//
// Restore net state not covered by register read/write API
//
void riscvNetRestore(
    riscvP              riscv,
    vmiRestoreContextP  cxt,
    vmiSaveRestorePhase phase
) {
    if(phase==SRT_END_CORE) {

        // restore pending interrupt state
        vmirtRestore(cxt, "ip", riscv->ip, riscv->ipDWords*8);

        // restore latched control input state
        VMIRT_RESTORE_FIELD(cxt, riscv, netValue);
        VMIRT_RESTORE_FIELD(cxt, riscv, intState);

        // refresh core state
        riscvTestInterrupt(riscv);
    }
}

//
// Save timer state not covered by register read/write API
//
void riscvTimerSave(
    riscvP              riscv,
    vmiSaveContextP     cxt,
    vmiSaveRestorePhase phase
) {
    if(phase==SRT_END_CORE) {

        if(riscv->stepTimer) {
            vmirtSaveModelTimer(cxt, "stepTimer", riscv->stepTimer);
        }
    }
}

//
// Restore timer state not covered by register read/write API
//
void riscvTimerRestore(
    riscvP              riscv,
    vmiRestoreContextP  cxt,
    vmiSaveRestorePhase phase
) {
    if(phase==SRT_END_CORE) {

        if(riscv->stepTimer) {
            vmirtRestoreModelTimer(cxt, "stepTimer", riscv->stepTimer);
        }
    }
}

