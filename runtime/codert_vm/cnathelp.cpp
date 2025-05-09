/*******************************************************************************
 * Copyright IBM Corp. and others 1991
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "j9.h"
#include "j9protos.h"
#include "j9consts.h"
#include "stackwalk.h"
#include "j9vmnls.h"
#include "objhelp.h"
#include "jitprotos.h"
#include "VMHelpers.hpp"
#include "VMAccess.hpp"
#include "AtomicSupport.hpp"
#include "JITInterface.hpp"
#include "ObjectAccessBarrierAPI.hpp"
#include "MethodMetaData.h"
#include "ut_j9codertvm.h"
#if JAVA_SPEC_VERSION >= 24
#include "ContinuationHelpers.hpp"
#endif /* JAVA_SPEC_VERSION >= 24 */

#undef DEBUG

extern "C" {

void* J9FASTCALL
old_slow_jitThrowNullPointerException(J9VMThread *currentThread);

J9_EXTERN_BUILDER_SYMBOL(throwCurrentExceptionFromJIT);
J9_EXTERN_BUILDER_SYMBOL(handlePopFramesFromJIT);
#if JAVA_SPEC_VERSION >= 24
J9_EXTERN_BUILDER_SYMBOL(yieldAtMonitorEnter);
#endif /* JAVA_SPEC_VERSION >= 24 */
#define J9_JITHELPER_ACTION_THROW		J9_BUILDER_SYMBOL(throwCurrentExceptionFromJIT)
#define J9_JITHELPER_ACTION_POP_FRAMES	J9_BUILDER_SYMBOL(handlePopFramesFromJIT)
#if JAVA_SPEC_VERSION >= 24
#define J9_JITHELPER_ACTION_YIELD_AT_MONENT	J9_BUILDER_SYMBOL(yieldAtMonitorEnter)
#endif /* JAVA_SPEC_VERSION >= 24 */

J9_EXTERN_BUILDER_SYMBOL(jitRunOnJavaStack);
#define JIT_RUN_ON_JAVA_STACK(x) (currentThread->tempSlot = (UDATA)(x), J9_BUILDER_SYMBOL(jitRunOnJavaStack))

#if defined(J9ZOS390) && !defined(J9VM_ENV_DATA64)
/* There is a bug in ZOS xlC which fails to mask one of the PCs if this is done inline */
static VMINLINE bool
samePCs(void *pc1, void *pc2)
{
	UDATA x = (UDATA)pc1 ^ (UDATA)pc2;
	return (0 == x) || (0x80000000 == x);
}
#else /* J9ZOS390 && !J9VM_ENV_DATA64 */
#define samePCs(pc1, pc2) (MASK_PC(pc1) == MASK_PC(pc2))
#endif /* J9ZOS390 && !J9VM_ENV_DATA64 */

/**
 * Fix the java and decompilation stacks for cases where exceptions can be
 * thrown from insde a JIT synthetic exception handler. There must be a
 * resolve frame on the top of the java stack with a valid JIT PC.
 *
 * @param currentThread[in] the current J9VMThread
 */
static void
fixStackForSyntheticHandler(J9VMThread *currentThread)
{
	/* If the top decompilation record is for the current compiled frame, re-link the resolve frame
	 * and decompilation record to keep the stack walkable.
	 */
	J9JITDecompilationInfo *decomp = currentThread->decompilationStack;
	if (NULL != decomp) {
		J9SFJITResolveFrame *resolveFrame = (J9SFJITResolveFrame*)currentThread->sp;
		void *jitPC = MASK_PC(resolveFrame->returnAddress);
		J9JITExceptionTable *metaData = jitGetExceptionTableFromPC(currentThread, (UDATA)jitPC);
		Assert_CodertVM_false(NULL == metaData);
		UDATA *oldSP = (UDATA*)(resolveFrame + 1);
		UDATA *bp = oldSP + getJitTotalFrameSize(metaData);
		if (bp == decomp->bp) {
			/* Use a value which is guaranteed to fail metadata lookup. This makes
			 * the stack walker get the PC from the decompilation stack.
			 */
			resolveFrame->returnAddress = NULL;
			decomp->pcAddress = (U_8**)&(resolveFrame->returnAddress);
			decomp->pc = (U_8*)jitPC;
		}
	}
}

/**
 * Determine the vTable offset for an interface send to a particular
 * receiver. If iTableOffset indicates a direct method, an assertion will
 * fire.
 *
 * @param currentThread[in] the current J9VMThread
 * @param receiverClass[in] the J9Class of the receiver
 * @param interfaceClass[in] the J9Class of the interface
 * @param iTableOffset[in] the iTable offset
 *
 * @returns the vTable index (0 indicates the mapping failed)
 */
static VMINLINE UDATA
convertITableOffsetToVTableOffset(J9VMThread *currentThread, J9Class *receiverClass, J9Class *interfaceClass, UDATA iTableOffset)
{
	UDATA vTableOffset = 0;
	J9ITable * iTable = receiverClass->lastITable;
	if (interfaceClass == iTable->interfaceClass) {
		goto foundITable;
	}

	iTable = (J9ITable*)receiverClass->iTable;
	while (NULL != iTable) {
		if (interfaceClass == iTable->interfaceClass) {
			receiverClass->lastITable = iTable;
foundITable:
			if (J9_UNEXPECTED(J9_ARE_ANY_BITS_SET(iTableOffset, J9_ITABLE_OFFSET_TAG_BITS))) {
				/* Direct methods should not reach here - no possibility of obtaining a vTableOffset */
				Assert_CodertVM_false(J9_ARE_ANY_BITS_SET(iTableOffset, J9_ITABLE_OFFSET_DIRECT));
				/* Object method in the vTable */
				vTableOffset = iTableOffset & ~J9_ITABLE_OFFSET_TAG_BITS;
			} else {
				/* Standard interface method */
				vTableOffset = *(UDATA*)(((UDATA)iTable) + iTableOffset);
			}
			goto done;
		}
		iTable = iTable->next;
	}
done:
	return vTableOffset;
}

static VMINLINE void*
buildJITResolveFrameWithPC(J9VMThread *currentThread, UDATA flags, UDATA parmCount, bool checkScavengeOnResolve, UDATA spAdjust, void *oldPC)
{
	VM_JITInterface::disableRuntimeInstrumentation(currentThread);
	oldPC = VM_VMHelpers::buildJITResolveFrameWithPC(currentThread, flags, parmCount, spAdjust, oldPC);
#if defined(J9VM_JIT_GC_ON_RESOLVE_SUPPORT) && defined(J9VM_GC_GENERATIONAL)
	if (checkScavengeOnResolve) {
		if (J9_ARE_ANY_BITS_SET(currentThread->javaVM->jitConfig->runtimeFlags, J9JIT_SCAVENGE_ON_RESOLVE)) {
			jitCheckScavengeOnResolve(currentThread);
		}
	}
#endif /* J9VM_JIT_GC_ON_RESOLVE_SUPPORT && J9VM_GC_GENERATIONAL */
	return oldPC;
}

void
buildBranchJITResolveFrame(J9VMThread *currentThread, void *pc, UDATA flags)
{
	buildJITResolveFrameWithPC(currentThread, flags | J9_SSF_JIT_RESOLVE, 0, false, 0, pc);
}

static VMINLINE void*
restoreJITResolveFrame(J9VMThread *currentThread, void *oldPC, bool checkAsync = true, bool checkException = true)
{
	void* addr = NULL;
	J9SFJITResolveFrame *resolveFrame = (J9SFJITResolveFrame*)currentThread->sp;
	if (checkAsync) {
		if (VM_VMHelpers::immediateAsyncPending(currentThread)) {
			if (J9_CHECK_ASYNC_POP_FRAMES == currentThread->javaVM->internalVMFunctions->javaCheckAsyncMessages(currentThread, FALSE)) {
				addr = J9_JITHELPER_ACTION_POP_FRAMES;
				goto done;
			}
		}
	}
	if (checkException) {
		if (VM_VMHelpers::exceptionPending(currentThread)) {
			addr = J9_JITHELPER_ACTION_THROW;
			goto done;
		}
	}
	if (NULL != oldPC) {
		void *newPC = resolveFrame->returnAddress;
		if (!samePCs(oldPC, newPC)) {
			addr = JIT_RUN_ON_JAVA_STACK(newPC);
			goto done;
		}
	}
	currentThread->jitException = resolveFrame->savedJITException;
	currentThread->sp = (UDATA*)(resolveFrame + 1);
	VM_JITInterface::enableRuntimeInstrumentation(currentThread);
done:
	return addr;
}

void
restoreBranchJITResolveFrame(J9VMThread *currentThread)
{
	restoreJITResolveFrame(currentThread, NULL, false, false);
}

extern void jitAddPicToPatchOnClassUnload(void *classPointer, void *addressToBePatched);

J9_EXTERN_BUILDER_SYMBOL(jitTranslateNewInstanceMethod);
J9_EXTERN_BUILDER_SYMBOL(jitInterpretNewInstanceMethod);
J9_EXTERN_BUILDER_SYMBOL(icallVMprJavaSendStatic1);

#define STATIC_FIELD_REF_BIT(bit) ((IDATA)(bit) << ((8 * sizeof(UDATA)) - J9_REQUIRED_CLASS_SHIFT))

#if defined(J9VM_ARCH_X86)

/* x86 common */

#define JIT_RETURN_ADDRESS ((void*)(currentThread->sp[0]))

#if defined(J9VM_ENV_DATA64)

/* x86-64 */

#define JIT_STACK_OVERFLOW_SIZE_REGISTER gpr.named.rdi
#define JIT_J2I_METHOD_REGISTER gpr.named.rdi
#define JIT_UDATA_RETURN_VALUE_REGISTER gpr.named.rax

#else /*J9VM_ENV_DATA64 */

/* x86-32*/

#define JIT_STACK_OVERFLOW_SIZE_REGISTER gpr.named.edi
#define JIT_J2I_METHOD_REGISTER gpr.named.edi
#define JIT_UDATA_RETURN_VALUE_REGISTER gpr.named.eax
#define JIT_LO_U64_RETURN_REGISTER gpr.named.eax
#define JIT_HI_U64_RETURN_REGISTER gpr.named.edx

#endif /* J9VM_ENV_DATA64 */

#elif defined(J9VM_ARCH_POWER)

#define JIT_STACK_OVERFLOW_SIZE_REGISTER gpr.numbered[12]
#define JIT_UDATA_RETURN_VALUE_REGISTER gpr.numbered[3]
#define JIT_RETURN_ADDRESS_REGISTER lr
#define JIT_J2I_METHOD_REGISTER gpr.numbered[3]
#if defined(J9VM_ENV_LITTLE_ENDIAN)
#define JIT_LO_U64_RETURN_REGISTER gpr.numbered[3]
#define JIT_HI_U64_RETURN_REGISTER gpr.numbered[4]
#else
#define JIT_LO_U64_RETURN_REGISTER gpr.numbered[4]
#define JIT_HI_U64_RETURN_REGISTER gpr.numbered[3]
#endif

#elif defined(J9VM_ARCH_S390)

#define JIT_STACK_OVERFLOW_SIZE_REGISTER gpr.numbered[0]
#define JIT_UDATA_RETURN_VALUE_REGISTER gpr.numbered[2]
#define JIT_RETURN_ADDRESS_REGISTER gpr.numbered[14]
#define JIT_J2I_METHOD_REGISTER gpr.numbered[1]
#define JIT_LO_U64_RETURN_REGISTER gpr.numbered[3]
#define JIT_HI_U64_RETURN_REGISTER gpr.numbered[2]

#elif defined(J9VM_ARCH_ARM)

#define JIT_STACK_OVERFLOW_SIZE_REGISTER gpr.numbered[11]
#define JIT_UDATA_RETURN_VALUE_REGISTER gpr.numbered[0]
#define JIT_RETURN_ADDRESS_REGISTER gpr.numbered[14]
#define JIT_J2I_METHOD_REGISTER gpr.numbered[0]
#define JIT_LO_U64_RETURN_REGISTER gpr.numbered[0]
#define JIT_HI_U64_RETURN_REGISTER gpr.numbered[1]

#elif defined(J9VM_ARCH_AARCH64)

#define JIT_STACK_OVERFLOW_SIZE_REGISTER gpr.numbered[9]
#define JIT_UDATA_RETURN_VALUE_REGISTER gpr.numbered[0]
#define JIT_RETURN_ADDRESS_REGISTER gpr.numbered[30]
#define JIT_J2I_METHOD_REGISTER gpr.numbered[0]

#elif defined(J9VM_ARCH_RISCV)

#define JIT_STACK_OVERFLOW_SIZE_REGISTER gpr.named.T6
#define JIT_UDATA_RETURN_VALUE_REGISTER gpr.named.A0
#define JIT_RETURN_ADDRESS_REGISTER gpr.named.RA
#define JIT_J2I_METHOD_REGISTER gpr.named.A0

#else
#error UNKNOWN PROCESSOR
#endif

#if defined(J9SW_NEEDS_JIT_2_INTERP_CALLEE_ARG_POP)
/* Entirely stack-based arguments */
#define JIT_PARM(number) (currentThread->sp[(number) - 1])
#define JIT_DIRECT_CALL_PARM(number) (currentThread->sp[parmCount - (number)])
#define SET_JIT_DIRECT_CALL_PARM(number, value) JIT_DIRECT_CALL_PARM(number) = (UDATA)(value)
#else /* J9SW_NEEDS_JIT_2_INTERP_CALLEE_ARG_POP */
/* Register-based arguments (with stack fallback if necessary) */
#if defined(JIT_RETURN_ADDRESS_REGISTER)
#define JIT_RETURN_ADDRESS ((void*)jitRegisters->JIT_RETURN_ADDRESS_REGISTER)
#endif /* JIT_RETURN_ADDRESS_REGISTER */
#define JIT_PARM_IN_REGISTER(number) (jitRegisters->gpr.numbered[jitArgumentRegisterNumbers[(number) - 1]])
#define JIT_PARM_IN_MEMORY(number) (currentThread->sp[parmCount - (number)])
#define JIT_PARM(number) ((number <= J9SW_ARGUMENT_REGISTER_COUNT) ? JIT_PARM_IN_REGISTER(number) : JIT_PARM_IN_MEMORY(number))
#define JIT_DIRECT_CALL_PARM(number) JIT_PARM(number)
#define SET_JIT_DIRECT_CALL_PARM(number, value) \
	do { \
		if (number <= J9SW_ARGUMENT_REGISTER_COUNT) { \
			JIT_PARM_IN_REGISTER(number) = (UDATA)(value); \
		} else { \
			JIT_PARM_IN_MEMORY(number) = (UDATA)(value); \
		} \
	} while(0)
#endif /* J9SW_NEEDS_JIT_2_INTERP_CALLEE_ARG_POP */

#define JIT_STACK_OVERFLOW_SIZE (jitRegisters->JIT_STACK_OVERFLOW_SIZE_REGISTER)

#define JIT_CLASS_PARM(number) ((J9Class*)JIT_PARM(number))
#define JIT_INT_PARM(number) ((I_32)JIT_PARM(number))

#define DECLARE_JIT_PARM(type, name, number) type const name = (type)JIT_PARM(number)
#define DECLARE_JIT_CLASS_PARM(name, number) J9Class* const name = JIT_CLASS_PARM(number)
#define DECLARE_JIT_INT_PARM(name, number) I_32 const name = JIT_INT_PARM(number)

#define JIT_RETURN_UDATA(value) currentThread->returnValue = (UDATA)(value)

#if defined(J9VM_ENV_DATA64)
#define DECLARE_JIT_U64_PARM(name, number) U_64 const name = (U_64)JIT_PARM(number)
#define JIT_RETURN_U64(value) JIT_RETURN_UDATA(value)
#else /* J9VM_ENV_DATA64 */
#define DECLARE_JIT_U64_PARM(name, number) U_64 const name = *(U_64*)&JIT_PARM(number)
#define JIT_RETURN_U64(value) \
	do { \
		((J9JITRegisters*)(currentThread->entryLocalStorage->jitGlobalStorageBase))->JIT_HI_U64_RETURN_REGISTER = (UDATA)((value) >> 32); \
		((J9JITRegisters*)(currentThread->entryLocalStorage->jitGlobalStorageBase))->JIT_LO_U64_RETURN_REGISTER = (UDATA)((value) & 0xFFFFFFFF); \
	} while(0)
#endif /* J9VM_ENV_DATA64 */

#if defined(J9VM_ARCH_X86)
#define PRESERVE_RETURN_ADDRESS() void* const x86ReturnAddress = currentThread->jitReturnAddress
#define RESTORE_RETURN_ADDRESS() currentThread->jitReturnAddress = x86ReturnAddress
#else
#define PRESERVE_RETURN_ADDRESS() do {} while(0)
#define RESTORE_RETURN_ADDRESS() do {} while(0)
#endif

#if defined(DEBUG)
#define JIT_HELPER_PROLOGUE() printf("\n" J9_GET_CALLSITE() "\n")
#else
#define JIT_HELPER_PROLOGUE() do {} while(0)
#endif

#if defined(J9SW_JIT_HELPERS_PASS_PARAMETERS_ON_STACK)
#define SET_PARM_COUNT(count) currentThread->floatTemp4 = (void*)count
#else
#define SET_PARM_COUNT(count) do {} while(0)
#endif /* J9SW_JIT_HELPERS_PASS_PARAMETERS_ON_STACK */

#define OLD_JIT_HELPER_PROLOGUE(count) \
	J9JITRegisters* const jitRegisters = (J9JITRegisters*)(currentThread->entryLocalStorage->jitGlobalStorageBase); \
	UDATA const parmCount = (count); \
	SET_PARM_COUNT(count); \
	JIT_HELPER_PROLOGUE()

#define OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(count) \
	PRESERVE_RETURN_ADDRESS(); \
	OLD_JIT_HELPER_PROLOGUE(count)

#define SLOW_JIT_HELPER_PROLOGUE() \
	UDATA const parmCount = (UDATA)currentThread->floatTemp4; \
	PRESERVE_RETURN_ADDRESS(); \
	JIT_HELPER_PROLOGUE()

#define SLOW_JIT_HELPER_EPILOGUE() RESTORE_RETURN_ADDRESS()

#if defined(J9VM_ARCH_X86) && !defined(J9VM_ENV_DATA64)
/* x86-32*/
extern void clearFPStack(void);
#define TIDY_BEFORE_THROW() clearFPStack()
#else /* defined(J9VM_ARCH_X86) && !defined(J9VM_ENV_DATA64) */
#define TIDY_BEFORE_THROW()
#endif /* defined(J9VM_ARCH_X86) && !defined(J9VM_ENV_DATA64) */

static VMINLINE void*
setCurrentExceptionFromJIT(J9VMThread *currentThread, UDATA exceptionNumber, j9object_t detailMessage)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setCurrentException(currentThread, exceptionNumber, (UDATA*)detailMessage);
	return J9_JITHELPER_ACTION_THROW;
}

static VMINLINE void*
setNegativeArraySizeExceptionFromJIT(J9VMThread *currentThread, I_32 size)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setNegativeArraySizeException(currentThread, size);
	return J9_JITHELPER_ACTION_THROW;
}

static VMINLINE void*
setCurrentExceptionNLSFromJIT(J9VMThread *currentThread, UDATA exceptionNumber, U_32 moduleName, U_32 messageNumber)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setCurrentExceptionNLS(currentThread, exceptionNumber, moduleName, messageNumber);
	return J9_JITHELPER_ACTION_THROW;
}

static VMINLINE void*
setCurrentExceptionUTFFromJIT(J9VMThread *currentThread, UDATA exceptionNumber, const char* errorUTF)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setCurrentExceptionUTF(currentThread, exceptionNumber, errorUTF);
	return J9_JITHELPER_ACTION_THROW;
}

static VMINLINE void*
setNativeOutOfMemoryErrorFromJIT(J9VMThread *currentThread, U_32 moduleName, U_32 messageNumber)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setNativeOutOfMemoryError(currentThread, moduleName, messageNumber);
	return J9_JITHELPER_ACTION_THROW;
}

#if defined(J9VM_OPT_CRIU_SUPPORT)
static VMINLINE void*
setCRIUSingleThreadModeExceptionFromJIT(J9VMThread *currentThread, U_32 moduleName, U_32 messageNumber)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setCRIUSingleThreadModeJVMCRIUException(currentThread, moduleName, messageNumber);
	return J9_JITHELPER_ACTION_THROW;
}
#endif /* defined(J9VM_OPT_CRIU_SUPPORT) */

static VMINLINE void*
setHeapOutOfMemoryErrorFromJIT(J9VMThread *currentThread)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setHeapOutOfMemoryError(currentThread);
	return J9_JITHELPER_ACTION_THROW;
}

static VMINLINE void*
setClassCastExceptionFromJIT(J9VMThread *currentThread, J9Class *instanceClass, J9Class *castClass)
{
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setClassCastException(currentThread, instanceClass, castClass);
	return J9_JITHELPER_ACTION_THROW;
}

static VMINLINE void*
buildJITResolveFrame(J9VMThread *currentThread, UDATA flags, UDATA helperParmCount, bool checkScavengeOnResolve = true, UDATA spAdjust = 0)
{
	void *oldPC = currentThread->jitReturnAddress;
	return buildJITResolveFrameWithPC(currentThread, flags, helperParmCount, checkScavengeOnResolve, spAdjust, oldPC);
}

static VMINLINE void*
buildJITResolveFrameForRuntimeHelper(J9VMThread *currentThread, UDATA helperParmCount)
{
	return buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE_RUNTIME_HELPER, helperParmCount);
}

static VMINLINE void*
buildJITResolveFrameForTrapHandler(J9VMThread *currentThread)
{
	/* Trap PC stored in jitException */
	void *oldPC = (void*)currentThread->jitException;
	currentThread->jitException = NULL;
	return buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE, 0, true, 0, oldPC);
}

static VMINLINE void
buildJITResolveFrameForRuntimeCheck(J9VMThread *currentThread)
{
	void *oldPC = currentThread->jitReturnAddress;
#if defined(J9VM_ARCH_X86)
	U_8 *pc = (U_8*)oldPC;
	I_32 offset = *(I_32*)pc;
	pc -= offset; /* compute addr of throwing instruction */
	pc += 1; /* move forward by a byte as codegen uses the beginning of instructions */
	oldPC = (void*)pc;
#endif /* J9VM_ARCH_X86 */
	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE, 0, true, 0, oldPC);
}


static VMINLINE bool
fast_jitNewObjectImpl(J9VMThread *currentThread, J9Class *objectClass, bool checkClassInit, bool nonZeroTLH)
{
	bool slowPathRequired = false;
	if (checkClassInit) {
		if (J9_UNEXPECTED(VM_VMHelpers::classRequiresInitialization(currentThread, objectClass))) {
			goto slow;
		}
	}
	if (J9_UNEXPECTED(!J9ROMCLASS_ALLOCATES_VIA_NEW(objectClass->romClass))) {
		goto slow;
	}
	{
		UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
		if (nonZeroTLH) {
			allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
		}
		j9object_t obj = currentThread->javaVM->memoryManagerFunctions->J9AllocateObjectNoGC(currentThread, objectClass, allocationFlags);
		if (J9_UNEXPECTED(NULL == obj)) {
			goto slow;
		}
		JIT_RETURN_UDATA(obj);
	}
done:
	return slowPathRequired;
slow:
	currentThread->floatTemp1 = (void*)objectClass;
	slowPathRequired = true;
	goto done;
}

static VMINLINE bool
fast_jitNewValueImpl(J9VMThread *currentThread, J9Class *objectClass, bool checkClassInit, bool nonZeroTLH)
{
	bool slowPathRequired = false;
	if (checkClassInit) {
		if (J9_UNEXPECTED(VM_VMHelpers::classRequiresInitialization(currentThread, objectClass))) {
			goto slow;
		}
	}
	if (J9_UNEXPECTED(!J9_IS_J9CLASS_VALUETYPE(objectClass))) {
		goto slow;
	}
	{
		UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
		if (nonZeroTLH) {
			allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
		}
		j9object_t obj = currentThread->javaVM->memoryManagerFunctions->J9AllocateObjectNoGC(currentThread, objectClass, allocationFlags);
		if (J9_UNEXPECTED(NULL == obj)) {
			goto slow;
		}
		JIT_RETURN_UDATA(obj);
	}
done:
	return slowPathRequired;
slow:
	currentThread->floatTemp1 = (void*)objectClass;
	slowPathRequired = true;
	goto done;
}

static VMINLINE void*
slow_jitNewObjectImpl(J9VMThread *currentThread, bool checkClassInit, bool nonZeroTLH)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9Class *objectClass = (J9Class*)currentThread->floatTemp1;
	j9object_t obj = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	void *addr = NULL;
	UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
	if (nonZeroTLH) {
		allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
	}
	if (!J9ROMCLASS_ALLOCATES_VIA_NEW(objectClass->romClass)) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGINSTANTIATIONERROR | J9_EX_CTOR_CLASS, J9VM_J9CLASS_TO_HEAPCLASS(objectClass));
		goto done;
	}
	if (checkClassInit) {
		if (VM_VMHelpers::classRequiresInitialization(currentThread, objectClass)) {
			buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			currentThread->javaVM->internalVMFunctions->initializeClass(currentThread, objectClass);
			addr = restoreJITResolveFrame(currentThread, oldPC);
			if (NULL != addr) {
				goto done;
			}
		}
	}
	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	obj = currentThread->javaVM->memoryManagerFunctions->J9AllocateObject(currentThread, objectClass, allocationFlags);
	if (NULL == obj) {
		addr = setHeapOutOfMemoryErrorFromJIT(currentThread);
		goto done;
	}
	currentThread->floatTemp1 = (void*)obj; // in case of decompile
	addr = restoreJITResolveFrame(currentThread, oldPC, false, false);
	if (NULL != addr) {
		goto done;
	}
	JIT_RETURN_UDATA(obj);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

static VMINLINE void*
slow_jitNewValueImpl(J9VMThread *currentThread, bool checkClassInit, bool nonZeroTLH)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9Class *objectClass = (J9Class*)currentThread->floatTemp1;
	j9object_t obj = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	void *addr = NULL;
	UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
	if (nonZeroTLH) {
		allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
	}
	if (!J9_IS_J9CLASS_VALUETYPE(objectClass)) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGINSTANTIATIONERROR | J9_EX_CTOR_CLASS, J9VM_J9CLASS_TO_HEAPCLASS(objectClass));
		goto done;
	}
	if (checkClassInit) {
		if (VM_VMHelpers::classRequiresInitialization(currentThread, objectClass)) {
			buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			currentThread->javaVM->internalVMFunctions->initializeClass(currentThread, objectClass);
			addr = restoreJITResolveFrame(currentThread, oldPC);
			if (NULL != addr) {
				goto done;
			}
		}
	}
	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	obj = currentThread->javaVM->memoryManagerFunctions->J9AllocateObject(currentThread, objectClass, allocationFlags);
	if (NULL == obj) {
		addr = setHeapOutOfMemoryErrorFromJIT(currentThread);
		goto done;
	}
	currentThread->floatTemp1 = (void*)obj; // in case of decompile
	addr = restoreJITResolveFrame(currentThread, oldPC, false, false);
	if (NULL != addr) {
		goto done;
	}
	JIT_RETURN_UDATA(obj);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}


void* J9FASTCALL
old_slow_jitNewObject(J9VMThread *currentThread)
{
	return slow_jitNewObjectImpl(currentThread, true, false);
}

void* J9FASTCALL
old_slow_jitNewValue(J9VMThread *currentThread)
{
	return slow_jitNewValueImpl(currentThread, true, false);
}

void* J9FASTCALL
old_fast_jitNewObject(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_CLASS_PARM(objectClass, 1);
	void *slowPath = NULL;
	if (fast_jitNewObjectImpl(currentThread, objectClass, true, false)) {
		slowPath = (void*)old_slow_jitNewObject;
	}
	return slowPath;
}

void* J9FASTCALL
old_slow_jitNewObjectNoZeroInit(J9VMThread *currentThread)
{
	return slow_jitNewObjectImpl(currentThread, true, true);
}

void* J9FASTCALL
old_slow_jitNewValueNoZeroInit(J9VMThread *currentThread)
{
	return slow_jitNewValueImpl(currentThread, true, true);
}

void* J9FASTCALL
old_fast_jitNewObjectNoZeroInit(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_CLASS_PARM(objectClass, 1);
	void *slowPath = NULL;
	if (fast_jitNewObjectImpl(currentThread, objectClass, true, true)) {
		slowPath = (void*)old_slow_jitNewObjectNoZeroInit;
	}
	return slowPath;
}

void* J9FASTCALL
old_slow_jitGetFlattenableField(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9RAMFieldRef* cpEntry = (J9RAMFieldRef*) currentThread->floatTemp1;
	j9object_t receiver = (j9object_t) currentThread->floatTemp2;
	j9object_t returnObject = NULL;
	void *rc = NULL;
	void *oldPC = currentThread->jitReturnAddress;

	if (NULL == receiver) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		rc = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
		goto done;
	}

	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	returnObject = currentThread->javaVM->internalVMFunctions->getFlattenableField(currentThread, cpEntry, receiver, FALSE);
	if (NULL == returnObject) {
		rc = setHeapOutOfMemoryErrorFromJIT(currentThread);
		goto done;
	}

	currentThread->floatTemp1 = (void*)returnObject; // in case of decompile
	rc = restoreJITResolveFrame(currentThread, oldPC, false, false);
	if (NULL != rc) {
		goto done;
	}

	JIT_RETURN_UDATA(returnObject);

done:
	SLOW_JIT_HELPER_EPILOGUE();
	return rc;
}

void* J9FASTCALL
old_fast_jitGetFlattenableField(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9RAMFieldRef*, cpEntry, 1);
	DECLARE_JIT_PARM(j9object_t, receiver, 2);
	j9object_t returnObject = NULL;
	void *rc = NULL;

	if (NULL == receiver) {
		goto slow;
	}

	returnObject = currentThread->javaVM->internalVMFunctions->getFlattenableField(currentThread, cpEntry, receiver, TRUE);
	if (J9_UNEXPECTED(NULL == returnObject)) {
		goto slow;
	}
	JIT_RETURN_UDATA(returnObject);

done:
	return rc;

slow:
	currentThread->floatTemp1 = (void*)cpEntry;
	currentThread->floatTemp2 = (void*)receiver;
	rc = (void *) old_slow_jitGetFlattenableField;
	goto done;
}

void* J9FASTCALL
old_slow_jitCloneValueType(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	j9object_t reciever = (j9object_t)currentThread->floatTemp1;
	j9object_t returnObject = NULL;
	void *rc = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	J9InternalVMFunctions *vmFuncs = currentThread->javaVM->internalVMFunctions;

	if (NULL == reciever) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		rc = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
		goto done;
	}

	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	returnObject = vmFuncs->cloneValueType(currentThread, J9OBJECT_CLAZZ(currentThread, reciever), reciever, FALSE);
	if (J9_UNEXPECTED(NULL == returnObject)) {
		rc = setHeapOutOfMemoryErrorFromJIT(currentThread);
		goto done;
	}

	currentThread->floatTemp1 = (void*)returnObject; // in case of decompile
	rc = restoreJITResolveFrame(currentThread, oldPC, false, false);
	if (NULL != rc) {
		goto done;
	}

	JIT_RETURN_UDATA(returnObject);

done:
	SLOW_JIT_HELPER_EPILOGUE();
	return rc;

}

void* J9FASTCALL
old_fast_jitCloneValueType(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, reciever, 1);
	j9object_t returnObject = NULL;
	void *rc = NULL;
	J9InternalVMFunctions *vmFuncs = currentThread->javaVM->internalVMFunctions;

	if (NULL == reciever) {
		goto slow;
	}

	returnObject = vmFuncs->cloneValueType(currentThread, J9OBJECT_CLAZZ(currentThread, reciever), reciever, TRUE);
	if (J9_UNEXPECTED(NULL == returnObject)) {
		goto slow;
	}

	JIT_RETURN_UDATA(returnObject);

done:
	return rc;

slow:
	currentThread->floatTemp1 = (void*)reciever;
	rc = (void*)old_slow_jitCloneValueType;
	goto done;
}

void* J9FASTCALL
old_slow_jitWithFlattenableField(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9RAMFieldRef* cpEntry = (J9RAMFieldRef*) currentThread->floatTemp1;
	j9object_t receiver = (j9object_t) currentThread->floatTemp2;
	j9object_t paramObject = (j9object_t) currentThread->floatTemp3;
	j9object_t returnObject = NULL;
	void *rc = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	J9InternalVMFunctions *vmFuncs = currentThread->javaVM->internalVMFunctions;

	if (NULL == receiver) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		rc = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
		goto done;
	}

	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	returnObject = vmFuncs->cloneValueType(currentThread, J9OBJECT_CLAZZ(currentThread, receiver), receiver, FALSE);
	if (J9_UNEXPECTED(NULL == returnObject)) {
		rc = setHeapOutOfMemoryErrorFromJIT(currentThread);
		goto done;
	}

	vmFuncs->putFlattenableField(currentThread, cpEntry, returnObject, paramObject);

	currentThread->floatTemp1 = (void*)returnObject; // in case of decompile
	rc = restoreJITResolveFrame(currentThread, oldPC, false, false);
	if (NULL != rc) {
		goto done;
	}

	JIT_RETURN_UDATA(returnObject);

done:
	SLOW_JIT_HELPER_EPILOGUE();
	return rc;

}

void* J9FASTCALL
old_fast_jitWithFlattenableField(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9RAMFieldRef*, cpEntry, 1);
	DECLARE_JIT_PARM(j9object_t, receiver, 2);
	DECLARE_JIT_PARM(j9object_t, paramObject, 3);
	j9object_t returnObject = NULL;
	void *rc = NULL;
	J9InternalVMFunctions *vmFuncs = currentThread->javaVM->internalVMFunctions;

	if (NULL == receiver) {
		goto slow;
	}

	returnObject = vmFuncs->cloneValueType(currentThread, J9OBJECT_CLAZZ(currentThread, receiver), receiver, TRUE);
	if (J9_UNEXPECTED(NULL == returnObject)) {
		goto slow;
	}

	vmFuncs->putFlattenableField(currentThread, cpEntry, returnObject, paramObject);

	JIT_RETURN_UDATA(returnObject);

done:
	return rc;

slow:
	currentThread->floatTemp1 = (void*)cpEntry;
	currentThread->floatTemp2 = (void*)receiver;
	currentThread->floatTemp3 = (void*)paramObject;
	rc = (void *) old_slow_jitWithFlattenableField;
	goto done;
}

void* J9FASTCALL
old_slow_jitPutFlattenableField(J9VMThread *currentThread)
{
	/* can only get here if we are throwing an exception */
	SLOW_JIT_HELPER_PROLOGUE();
	void *rc = NULL;

	buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	rc = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);

	SLOW_JIT_HELPER_EPILOGUE();
	return rc;
}

void* J9FASTCALL
old_fast_jitPutFlattenableField(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9RAMFieldRef*, cpEntry, 1);
	DECLARE_JIT_PARM(j9object_t, receiver, 2);
	DECLARE_JIT_PARM(j9object_t, paramObject, 3);
	j9object_t returnObject = NULL;
	void *rc = NULL;

	if (NULL == receiver) {
		goto slow;
	}

	currentThread->javaVM->internalVMFunctions->putFlattenableField(currentThread, cpEntry, receiver, paramObject);

done:
	return rc;

slow:
	rc = (void *) old_slow_jitPutFlattenableField;
	goto done;
}

void* J9FASTCALL
old_slow_jitGetFlattenableStaticField(J9VMThread *currentThread)
{
	return NULL;
}

void* J9FASTCALL
old_fast_jitGetFlattenableStaticField(J9VMThread *currentThread)
{
	return NULL;
}

void* J9FASTCALL
old_slow_jitPutFlattenableStaticField(J9VMThread *currentThread)
{
	return NULL;
}

void* J9FASTCALL
old_fast_jitPutFlattenableStaticField(J9VMThread *currentThread)
{
	return NULL;
}

void* J9FASTCALL
old_slow_jitLoadFlattenableArrayElement(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	j9object_t arrayObject = (j9object_t)currentThread->floatTemp1;
	U_32 index = *(U_32 *)&currentThread->floatTemp2;
	void *addr = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	if (NULL == arrayObject) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
	} else {
		j9object_t value = NULL;
		U_32 arrayLength = J9INDEXABLEOBJECT_SIZE(currentThread, arrayObject);
		if (index >= arrayLength) {
			buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, NULL);
		} else {
			buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
			value = currentThread->javaVM->internalVMFunctions->loadFlattenableArrayElement(currentThread, arrayObject, index, false);
			if (NULL == value) {
				addr = setHeapOutOfMemoryErrorFromJIT(currentThread);
				goto done;
			}
			currentThread->floatTemp1 = (void*)value; // in case of decompile
			addr = restoreJITResolveFrame(currentThread, oldPC, false, false);
			if (NULL != addr) {
				goto done;
			}
			JIT_RETURN_UDATA(value);
		}
	}
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_fast_jitLoadFlattenableArrayElement(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(j9object_t, arrayObject, 1);
	DECLARE_JIT_PARM(U_32, index, 2);
	bool slowPathUsed = false;
	void *slowPath = NULL;
	j9object_t value = NULL;
	U_32 arrayLength = 0;
	if (NULL == arrayObject) {
		goto slow;
	}
	arrayLength = J9INDEXABLEOBJECT_SIZE(currentThread, arrayObject);
	if (index >= arrayLength) {
		goto slow;
	}
	value = (j9object_t) currentThread->javaVM->internalVMFunctions->loadFlattenableArrayElement(currentThread, arrayObject, index, true);
	if (NULL == value) {
		J9ArrayClass *arrayObjectClass = (J9ArrayClass *)J9OBJECT_CLAZZ(currentThread, arrayObject);
		if (J9_IS_J9ARRAYCLASS_NULL_RESTRICTED(arrayObjectClass)) {
			goto slow;
		}
	}
	JIT_RETURN_UDATA(value);
done:
	return slowPath;
slow:
	slowPathUsed = true;
	currentThread->floatTemp1 = (void *)arrayObject;
	currentThread->floatTemp2 = *(void **)&index;
	slowPath = (void*)old_slow_jitLoadFlattenableArrayElement;
	goto done;
}

void* J9FASTCALL
old_slow_jitStoreFlattenableArrayElement(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	j9object_t arrayref = (j9object_t)currentThread->floatTemp1;
	U_32 index = *(U_32 *)&currentThread->floatTemp2;
	j9object_t value = (j9object_t)currentThread->floatTemp3;
	U_32 arrayLength = 0;
	void *addr = NULL;
	buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	if (NULL == arrayref) {
		addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
	} else {
		arrayLength = J9INDEXABLEOBJECT_SIZE(currentThread, arrayref);
		if (index >= arrayLength) {
			addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, NULL);
		} else {
			if (false == VM_VMHelpers::objectArrayStoreAllowed(currentThread, arrayref, value)) {
				addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION, NULL);
			} else {
				J9ArrayClass *arrayrefClass = (J9ArrayClass *) J9OBJECT_CLAZZ(currentThread, arrayref);
				addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
			}
		}
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_fast_jitStoreFlattenableArrayElement(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(j9object_t, arrayref, 1);
	DECLARE_JIT_PARM(U_32, index, 2);
	DECLARE_JIT_PARM(j9object_t, value, 3);

	bool slowPathUsed = false;
	void *slowPath = NULL;
	U_32 arrayLength = 0;
	J9ArrayClass *arrayrefClass = NULL;
	if (NULL == arrayref) {
		goto slow;
	}
	arrayLength = J9INDEXABLEOBJECT_SIZE(currentThread, arrayref);
	if (index >= arrayLength) {
		goto slow;
	}
	if (false == VM_VMHelpers::objectArrayStoreAllowed(currentThread, arrayref, value)) {
		goto slow;
	}
	currentThread->javaVM->internalVMFunctions->storeFlattenableArrayElement(currentThread, arrayref, index, value);
done:
	return slowPath;
slow:
	slowPathUsed = true;
	currentThread->floatTemp1 = (void *)arrayref;
	currentThread->floatTemp2 = *(void **)&index;
	currentThread->floatTemp3 = (void *)value;
	slowPath = (void*)old_slow_jitStoreFlattenableArrayElement;
	goto done;
}

static VMINLINE bool
fast_jitANewArrayImpl(J9VMThread *currentThread, J9Class *elementClass, I_32 size, bool nonZeroTLH)
{
	bool slowPathRequired = false;
	J9Class * const arrayClass = elementClass->arrayClass;
	if (J9_UNEXPECTED(NULL == arrayClass)) {
		goto slow;
	}
	if (J9_UNEXPECTED(size < 0)) {
		goto slow;
	}
	{
		UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
		if (nonZeroTLH) {
			allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
		}
		j9object_t obj = currentThread->javaVM->memoryManagerFunctions->J9AllocateIndexableObjectNoGC(currentThread, arrayClass, (U_32)size, allocationFlags);
		if (J9_UNEXPECTED(NULL == obj)) {
			goto slow;
		}
		JIT_RETURN_UDATA(obj);
	}
done:
	return slowPathRequired;
slow:
	currentThread->floatTemp1 = (void*)(UDATA)elementClass;
	currentThread->floatTemp2 = (void*)(UDATA)size;
	slowPathRequired = true;
	goto done;
}

static VMINLINE void*
slow_jitANewArrayImpl(J9VMThread *currentThread, bool nonZeroTLH)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9Class *elementClass = (J9Class*)currentThread->floatTemp1;
	I_32 size = (I_32)(UDATA)currentThread->floatTemp2;
	J9Class *arrayClass = elementClass->arrayClass;
	j9object_t obj = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	void *addr = NULL;
	UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
	if (nonZeroTLH) {
		allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
	}
	if (size < 0) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		addr = setNegativeArraySizeExceptionFromJIT(currentThread, size);
		goto done;
	}
	if (NULL == arrayClass) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		J9JavaVM *vm = currentThread->javaVM;
		arrayClass = vm->internalVMFunctions->internalCreateArrayClass(currentThread, (J9ROMArrayClass*)J9ROMIMAGEHEADER_FIRSTCLASS(vm->arrayROMClasses), elementClass);
		addr = restoreJITResolveFrame(currentThread, oldPC);
		if (NULL != addr) {
			goto done;
		}
	}
	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	obj = currentThread->javaVM->memoryManagerFunctions->J9AllocateIndexableObject(currentThread, arrayClass, (U_32)size, allocationFlags);
	if (NULL == obj) {
		addr = setHeapOutOfMemoryErrorFromJIT(currentThread);
		goto done;
	}
	currentThread->floatTemp1 = (void*)obj; // in case of decompile
	addr = restoreJITResolveFrame(currentThread, oldPC, false, false);
	if (NULL != addr) {
		goto done;
	}
	JIT_RETURN_UDATA(obj);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitANewArray(J9VMThread *currentThread)
{
	return slow_jitANewArrayImpl(currentThread, false);
}

void* J9FASTCALL
old_fast_jitANewArray(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(elementClass, 1);
	DECLARE_JIT_INT_PARM(size, 2);
	void *slowPath = NULL;
	if (fast_jitANewArrayImpl(currentThread, elementClass, size, false)) {
		slowPath = (void*)old_slow_jitANewArray;
	}
	return slowPath;
}

void* J9FASTCALL
old_slow_jitANewArrayNoZeroInit(J9VMThread *currentThread)
{
	return slow_jitANewArrayImpl(currentThread, true);
}

void* J9FASTCALL
old_fast_jitANewArrayNoZeroInit(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(elementClass, 1);
	DECLARE_JIT_INT_PARM(size, 2);
	void *slowPath = NULL;
	if (fast_jitANewArrayImpl(currentThread, elementClass, size, true)) {
		slowPath = (void*)old_slow_jitANewArrayNoZeroInit;
	}
	return slowPath;
}

static VMINLINE bool
fast_jitNewArrayImpl(J9VMThread *currentThread, I_32 arrayType, I_32 size, bool nonZeroTLH)
{
	bool slowPathRequired = true;
	currentThread->floatTemp1 = (void*)(UDATA)arrayType;
	currentThread->floatTemp2 = (void*)(UDATA)size;
	if (size >= 0) {
		J9JavaVM *vm = currentThread->javaVM;
		J9Class *arrayClass = (&(vm->booleanArrayClass))[arrayType - 4];
		UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
		if (nonZeroTLH) {
			allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
		}
		j9object_t obj = vm->memoryManagerFunctions->J9AllocateIndexableObjectNoGC(currentThread, arrayClass, (U_32)size, allocationFlags);
		if (NULL != obj) {
			slowPathRequired = false;
			JIT_RETURN_UDATA(obj);
		}
	}
	return slowPathRequired;
}

static VMINLINE void*
slow_jitNewArrayImpl(J9VMThread *currentThread, bool nonZeroTLH)
{
	SLOW_JIT_HELPER_PROLOGUE();
	I_32 arrayType = (I_32)(UDATA)currentThread->floatTemp1;
	I_32 size = (I_32)(UDATA)currentThread->floatTemp2;
	J9JavaVM *vm = currentThread->javaVM;
	J9Class *arrayClass = (&(vm->booleanArrayClass))[arrayType - 4];
	j9object_t obj = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	void *addr = NULL;
	UDATA allocationFlags = J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE;
	if (nonZeroTLH) {
		allocationFlags |= J9_GC_ALLOCATE_OBJECT_NON_ZERO_TLH;
	}
	if (size < 0) {
		buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		addr = setNegativeArraySizeExceptionFromJIT(currentThread, size);
		goto done;
	}
	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	obj = vm->memoryManagerFunctions->J9AllocateIndexableObject(currentThread, arrayClass, (U_32)size, allocationFlags);
	if (NULL == obj) {
		addr = setHeapOutOfMemoryErrorFromJIT(currentThread);
		goto done;
	}
	currentThread->floatTemp1 = (void*)obj; // in case of decompile
	addr = restoreJITResolveFrame(currentThread, oldPC, false, false);
	if (NULL != addr) {
		goto done;
	}
	JIT_RETURN_UDATA(obj);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitNewArray(J9VMThread *currentThread)
{
	return slow_jitNewArrayImpl(currentThread, false);
}

void* J9FASTCALL
old_fast_jitNewArray(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_INT_PARM(arrayType, 1);
	DECLARE_JIT_INT_PARM(size, 2);
	void *slowPath = NULL;
	if (fast_jitNewArrayImpl(currentThread, arrayType, size, false)) {
		slowPath = (void*)old_slow_jitNewArray;
	}
	return slowPath;
}

void* J9FASTCALL
old_slow_jitNewArrayNoZeroInit(J9VMThread *currentThread)
{
	return slow_jitNewArrayImpl(currentThread, true);
}

void* J9FASTCALL
old_fast_jitNewArrayNoZeroInit(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_INT_PARM(arrayType, 1);
	DECLARE_JIT_INT_PARM(size, 2);
	void *slowPath = NULL;
	if (fast_jitNewArrayImpl(currentThread, arrayType, size, true)) {
		slowPath = (void*)old_slow_jitNewArrayNoZeroInit;
	}
	return slowPath;
}

void* J9FASTCALL
old_slow_jitAMultiNewArray(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_CLASS_PARM(elementClass, 1);
	DECLARE_JIT_INT_PARM(dimensions, 2);
	DECLARE_JIT_PARM(I_32*, dimensionsArray, 3);
	j9object_t obj = NULL;
	void *oldPC = currentThread->jitReturnAddress;
	void *addr = NULL;
	buildJITResolveFrameWithPC(currentThread, J9_STACK_FLAGS_JIT_ALLOCATION_RESOLVE | J9_SSF_JIT_RESOLVE, parmCount, true, 0, oldPC);
	obj = currentThread->javaVM->internalVMFunctions->helperMultiANewArray(currentThread, (J9ArrayClass*)elementClass, (UDATA)dimensions, dimensionsArray, J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE);
	currentThread->floatTemp1 = (void*)obj; // in case of decompile
	addr = restoreJITResolveFrame(currentThread, oldPC);
	if (NULL != addr) {
		goto done;
	}
	JIT_RETURN_UDATA(obj);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitStackOverflow(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(0);
	UDATA frameSize = JIT_STACK_OVERFLOW_SIZE;
	void *oldPC = currentThread->jitReturnAddress;
	void *addr = NULL;
	UDATA sp = (UDATA)currentThread->sp;
	UDATA checkSP = sp  - frameSize;
	if ((checkSP > sp) || VM_VMHelpers::shouldGrowForSP(currentThread, (UDATA*)checkSP)) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_STACK_OVERFLOW, parmCount, true, 0, oldPC);
		checkSP = (UDATA)currentThread->sp - frameSize;
		UDATA currentUsed = (UDATA)currentThread->stackObject->end - checkSP;
		J9JavaVM *vm = currentThread->javaVM;
		UDATA maxStackSize = vm->stackSize;
		if (currentUsed > maxStackSize) {
throwStackOverflow:
			if (currentThread->privateFlags & J9_PRIVATE_FLAGS_STACK_OVERFLOW) {
				vm->internalVMFunctions->fatalRecursiveStackOverflow(currentThread);
			}
			addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGSTACKOVERFLOWERROR, NULL);
			goto done;
		}
		currentUsed += vm->stackSizeIncrement;
		if (currentUsed > maxStackSize) {
			currentUsed = maxStackSize;
		}
		UDATA growRC = vm->internalVMFunctions->growJavaStack(currentThread, currentUsed);
		if (0 != growRC) {
			goto throwStackOverflow;
		}
		addr = restoreJITResolveFrame(currentThread, oldPC, false, false);
		if (NULL != addr) {
			goto done;
		}
	}
	if (VM_VMHelpers::asyncMessagePending(currentThread)) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_STACK_OVERFLOW, parmCount, true, 0, oldPC);
		UDATA asyncAction = currentThread->javaVM->internalVMFunctions->javaCheckAsyncMessages(currentThread, TRUE);
		switch(asyncAction) {
		case J9_CHECK_ASYNC_THROW_EXCEPTION:
			addr = J9_JITHELPER_ACTION_THROW;
			goto done;
		case J9_CHECK_ASYNC_POP_FRAMES:
			addr = J9_JITHELPER_ACTION_POP_FRAMES;
			goto done;
		}
		addr = restoreJITResolveFrame(currentThread, oldPC);
	}
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveString(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
	J9RAMStringRef *ramCPEntry = (J9RAMStringRef*)ramConstantPool + cpIndex;
	j9object_t volatile *stringObjectPtr = (j9object_t volatile *)&ramCPEntry->stringObject;
	if (NULL == *stringObjectPtr) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		currentThread->javaVM->internalVMFunctions->resolveStringRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
	}
	JIT_RETURN_UDATA(stringObjectPtr);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void J9FASTCALL
fast_jitAcquireVMAccess(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
#if defined(J9VM_INTERP_ATOMIC_FREE_JNI)
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
#else /* J9VM_INTERP_ATOMIC_FREE_JNI */
	VM_VMAccess::inlineAcquireVMAccessExhaustiveCheck(currentThread);
#endif /* J9VM_INTERP_ATOMIC_FREE_JNI */
}

void J9FASTCALL
fast_jitReleaseVMAccess(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
#if defined(J9VM_INTERP_ATOMIC_FREE_JNI)
	VM_VMAccess::inlineExitVMToJNI(currentThread);
#else /* J9VM_INTERP_ATOMIC_FREE_JNI */
	VM_VMAccess::inlineReleaseVMAccess(currentThread);
#endif /* J9VM_INTERP_ATOMIC_FREE_JNI */
}

void* J9FASTCALL
old_slow_jitCheckAsyncMessages(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(0);
	void *addr = NULL;
	if (VM_VMHelpers::asyncMessagePending(currentThread)) {
		void *oldPC = buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE, parmCount, false);
		if (J9_CHECK_ASYNC_POP_FRAMES == currentThread->javaVM->internalVMFunctions->javaCheckAsyncMessages(currentThread, FALSE)) {
			addr = J9_JITHELPER_ACTION_POP_FRAMES;
			goto done;
		}
		addr = restoreJITResolveFrame(currentThread, oldPC, false, false);
	}
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
impl_jitClassCastException(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9Class* castClass = (J9Class*)currentThread->floatTemp1;
	J9Class* instanceClass = (J9Class*)currentThread->floatTemp2;
	buildJITResolveFrameForRuntimeHelper(currentThread, 0);
	void* exception = setClassCastExceptionFromJIT(currentThread, instanceClass, castClass);
	SLOW_JIT_HELPER_EPILOGUE();
	return exception;
}

void* J9FASTCALL
old_slow_jitCheckCast(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9Class *castClass = (J9Class*)currentThread->floatTemp1;
	j9object_t object = (j9object_t)currentThread->floatTemp2;
	J9Class *instanceClass = J9OBJECT_CLAZZ(currentThread, object);
	buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	return setClassCastExceptionFromJIT(currentThread, instanceClass, castClass);
}

void* J9FASTCALL
old_fast_jitCheckCast(J9VMThread *currentThread)
{
	void *slowPath = NULL;
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(castClass, 1);
	DECLARE_JIT_PARM(j9object_t, object, 2);
	/* null can be cast to anything, except if castClass is a primitive VT */
	if (NULL != object) {
		J9Class *instanceClass = J9OBJECT_CLAZZ(currentThread, object);
		if (!VM_VMHelpers::inlineCheckCast(instanceClass, castClass)) {
			currentThread->floatTemp1 = (void*)castClass;
			currentThread->floatTemp2 = (void*)object;
			slowPath = (void*)old_slow_jitCheckCast;
		}
	}
	/* In the future, Valhalla checkcast must throw an exception on
	 * null-restricted checkedType if object is null.
	 * See issue https://github.com/eclipse-openj9/openj9/issues/19764.
	 */
	return slowPath;
}

void* J9FASTCALL
old_slow_jitCheckCastForArrayStore(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION, NULL);
}

void* J9FASTCALL
old_fast_jitCheckCastForArrayStore(J9VMThread *currentThread)
{
	void *slowPath = NULL;
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(castClass, 1);
	DECLARE_JIT_PARM(j9object_t, object, 2);
	/* null can be cast to anything, except if castClass is a primitive VT */
	if (NULL != object) {
		J9Class *instanceClass = J9OBJECT_CLAZZ(currentThread, object);
		if (!VM_VMHelpers::inlineCheckCast(instanceClass, castClass)) {
			slowPath = (void*)old_slow_jitCheckCastForArrayStore;
		}
	}
#if defined(J9VM_OPT_VALHALLA_FLATTENABLE_VALUE_TYPES)
	else if (J9_IS_J9CLASS_PRIMITIVE_VALUETYPE(castClass)) {
		slowPath = (void*)old_slow_jitThrowNullPointerException;
	}
#endif /* defined(J9VM_OPT_VALHALLA_FLATTENABLE_VALUE_TYPES) */
	return slowPath;
}

void J9FASTCALL
old_fast_jitCheckIfFinalizeObject(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, object, 1);
	VM_VMHelpers::checkIfFinalizeObject(currentThread, object);
}

void J9FASTCALL
old_fast_jitCollapseJNIReferenceFrame(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
	/* The direct JNI code has dropped all pushed JNI refs from the stack, and has called this routine on the java stack */
	UDATA *bp = ((UDATA *)(((J9SFJNINativeMethodFrame*)currentThread->sp) + 1)) - 1;
	currentThread->javaVM->internalVMFunctions->returnFromJNI(currentThread, bp);
}

void* J9FASTCALL
old_slow_jitHandleArrayIndexOutOfBoundsTrap(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
	buildJITResolveFrameForTrapHandler(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitHandleIntegerDivideByZeroTrap(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
	buildJITResolveFrameForTrapHandler(currentThread);
	return setCurrentExceptionNLSFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARITHMETICEXCEPTION, J9NLS_VM_DIVIDE_BY_ZERO);
}

void* J9FASTCALL
old_slow_jitHandleNullPointerExceptionTrap(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
	buildJITResolveFrameForTrapHandler(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitHandleInternalErrorTrap(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
	buildJITResolveFrameForTrapHandler(currentThread);
	return setCurrentExceptionUTFFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGINTERNALERROR, "SIGBUS");
}

void J9FASTCALL
old_fast_jitInstanceOf(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(castClass, 1);
	DECLARE_JIT_PARM(j9object_t, object, 2);
	UDATA isInstance = 0;
	/* null isn't an instance of anything */
	if (NULL != object) {
		J9Class *instanceClass = J9OBJECT_CLAZZ(currentThread, object);
		if (VM_VMHelpers::inlineCheckCast(instanceClass, castClass)) {
			isInstance = 1;
		}
	}
	JIT_RETURN_UDATA(isInstance);
}

void* J9FASTCALL
old_slow_jitLookupInterfaceMethod(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9Class *receiverClass = (J9Class*)currentThread->floatTemp1;
	UDATA *indexAndLiteralsEA = (UDATA*)currentThread->floatTemp2;
	void *jitEIP = (void*)currentThread->floatTemp3;
	J9Class *interfaceClass = ((J9Class**)indexAndLiteralsEA)[0];
	UDATA iTableOffset = indexAndLiteralsEA[1];
	UDATA vTableOffset = convertITableOffsetToVTableOffset(currentThread, receiverClass, interfaceClass, iTableOffset);
	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_INTERFACE_LOOKUP, parmCount, true, 0, jitEIP);
	if (0 == vTableOffset) {
		setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGINCOMPATIBLECLASSCHANGEERROR, NULL);
	} else {
		J9Method* method = *(J9Method**)((UDATA)receiverClass + vTableOffset);
		TIDY_BEFORE_THROW();
		currentThread->javaVM->internalVMFunctions->setIllegalAccessErrorNonPublicInvokeInterface(currentThread, method);
	}
	return J9_JITHELPER_ACTION_THROW;
}

void* J9FASTCALL
old_fast_jitLookupInterfaceMethod(J9VMThread *currentThread)
{
	void *slowPath = (void*)old_slow_jitLookupInterfaceMethod;
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_CLASS_PARM(receiverClass, 1);
	DECLARE_JIT_PARM(UDATA*, indexAndLiteralsEA, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	currentThread->floatTemp1 = (void*)receiverClass;
	currentThread->floatTemp2 = (void*)indexAndLiteralsEA;
	currentThread->floatTemp3 = (void*)jitEIP;
	J9Class *interfaceClass = ((J9Class**)indexAndLiteralsEA)[0];
	UDATA iTableOffset = indexAndLiteralsEA[1];
	UDATA vTableOffset = convertITableOffsetToVTableOffset(currentThread, receiverClass, interfaceClass, iTableOffset);
	if (0 != vTableOffset) {
		J9Method* method = *(J9Method**)((UDATA)receiverClass + vTableOffset);
		if (J9_ARE_ANY_BITS_SET(J9_ROM_METHOD_FROM_RAM_METHOD(method)->modifiers, J9AccPublic)) {
			slowPath = NULL;
			JIT_RETURN_UDATA(vTableOffset);
		}
	}
	return slowPath;
}

#if defined(J9VM_OPT_OPENJDK_METHODHANDLE)
void* J9FASTCALL
old_slow_jitLookupDynamicPublicInterfaceMethod(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9Method *method = (J9Method*)currentThread->floatTemp1;
	buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setIllegalAccessErrorNonPublicInvokeInterface(currentThread, method);
	return J9_JITHELPER_ACTION_THROW;
}

void* J9FASTCALL
old_fast_jitLookupDynamicPublicInterfaceMethod(J9VMThread *currentThread)
{
	void *slowPath = (void*)old_slow_jitLookupDynamicPublicInterfaceMethod;
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(receiverClass, 1);
	DECLARE_JIT_PARM(j9object_t, memberName, 2);
	J9JavaVM *vm = currentThread->javaVM;
	J9Method *interfaceMethod = (J9Method *)(UDATA)J9OBJECT_U64_LOAD(currentThread, memberName, vm->vmtargetOffset);
	J9Class *interfaceClass = J9_CLASS_FROM_METHOD(interfaceMethod);
	UDATA iTableIndex = (UDATA)J9OBJECT_U64_LOAD(currentThread, memberName, vm->vmindexOffset);
	UDATA iTableOffset = sizeof(struct J9ITable) + (iTableIndex * sizeof(UDATA));
	UDATA vTableOffset = convertITableOffsetToVTableOffset(currentThread, receiverClass, interfaceClass, iTableOffset);
	Assert_CodertVM_false(0 == vTableOffset);
	/* The receiver's implementation is required to be public */
	J9Method *method = *(J9Method**)((UDATA)receiverClass + vTableOffset);
	if (J9_ARE_ANY_BITS_SET(J9_ROM_METHOD_FROM_RAM_METHOD(method)->modifiers, J9AccPublic)) {
		slowPath = NULL;
		JIT_RETURN_UDATA(vTableOffset);
	} else {
		currentThread->floatTemp1 = (void*)method;
	}
	return slowPath;
}
#endif /* defined(J9VM_OPT_OPENJDK_METHODHANDLE) */

void J9FASTCALL
old_fast_jitMethodIsNative(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	UDATA isNative = 0;
	if (J9_ARE_ANY_BITS_SET(J9_ROM_METHOD_FROM_RAM_METHOD(method)->modifiers, J9AccNative)) {
		isNative = 1;
	}
	JIT_RETURN_UDATA(isNative);
}

void J9FASTCALL
old_fast_jitMethodIsSync(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	UDATA isSync = 0;
	if (J9_ARE_ANY_BITS_SET(J9_ROM_METHOD_FROM_RAM_METHOD(method)->modifiers, J9AccSynchronized)) {
		isSync = 1;
	}
	JIT_RETURN_UDATA(isSync);
}

static VMINLINE bool
fast_jitMonitorEnterImpl(J9VMThread *currentThread, j9object_t syncObject, bool forMethod)
{
	bool slowPathRequired = false;
	UDATA monstatus = currentThread->javaVM->internalVMFunctions->objectMonitorEnterNonBlocking(currentThread, syncObject);
	if (monstatus <= J9_OBJECT_MONITOR_BLOCKING) {
		slowPathRequired = true;
		currentThread->floatTemp1 = (void*)monstatus;
#if JAVA_SPEC_VERSION >= 16
		currentThread->floatTemp2 = (void*)syncObject;
#endif /* JAVA_SPEC_VERSION >= 16 */
	}
	return slowPathRequired;
}

static VMINLINE void*
slow_jitMonitorEnterImpl(J9VMThread *currentThread, bool forMethod)
{
	SLOW_JIT_HELPER_PROLOGUE();
	void *addr = NULL;
	UDATA flags = J9_STACK_FLAGS_JIT_RESOLVE_FRAME | (forMethod ? J9_STACK_FLAGS_JIT_METHOD_MONITOR_ENTER_RESOLVE : J9_STACK_FLAGS_JIT_MONITOR_ENTER_RESOLVE);
	IDATA monstatus = (IDATA)(UDATA)currentThread->floatTemp1;
	void *oldPC = buildJITResolveFrame(currentThread, flags, parmCount);
#if JAVA_SPEC_VERSION >= 24
	if ((J9_OBJECT_MONITOR_BLOCKING == monstatus) && VM_ContinuationHelpers::isYieldableVirtualThread(currentThread)) {
		/* Try to yield the virtual thread if it will be blocked. */
		monstatus = currentThread->javaVM->internalVMFunctions->preparePinnedVirtualThreadForUnmount(currentThread, (j9object_t)currentThread->floatTemp2, false);
		if (monstatus > J9_OBJECT_MONITOR_BLOCKING) {
			/* Monitor has been acquired. */
			addr = restoreJITResolveFrame(currentThread, oldPC, forMethod, false);
			goto done;
		}
	}
#endif /* JAVA_SPEC_VERSION >= 24 */
	if (monstatus < J9_OBJECT_MONITOR_BLOCKING) {
		if (forMethod) {
			/* Only mark the outer frame for failed method monitor enter - inline frames have correct maps */
			void * stackMap = NULL;
			void * inlineMap = NULL;
			J9JavaVM *vm = currentThread->javaVM;
			J9JITExceptionTable *metaData = vm->jitConfig->jitGetExceptionTableFromPC(currentThread, (UDATA)oldPC);
			Assert_CodertVM_false(NULL == metaData);
			jitGetMapsFromPC(currentThread, vm, metaData, (UDATA)oldPC, &stackMap, &inlineMap);
			Assert_CodertVM_false(NULL == inlineMap);
			if ((NULL == getJitInlinedCallInfo(metaData)) || (NULL == getFirstInlinedCallSite(metaData, inlineMap))) {
				J9SFJITResolveFrame *resolveFrame = (J9SFJITResolveFrame*)currentThread->sp;
				resolveFrame->specialFrameFlags = (resolveFrame->specialFrameFlags & ~J9_STACK_FLAGS_JIT_FRAME_SUB_TYPE_MASK) | J9_STACK_FLAGS_JIT_FAILED_METHOD_MONITOR_ENTER_RESOLVE;
			}
		}
		switch (monstatus) {
#if JAVA_SPEC_VERSION >= 16
		case J9_OBJECT_MONITOR_VALUE_TYPE_IMSE: {
			j9object_t syncObject = (j9object_t)currentThread->floatTemp2;
			J9Class* badClass = J9OBJECT_CLAZZ(currentThread, syncObject);
			J9UTF8 *className = J9ROMCLASS_CLASSNAME(badClass->romClass);
			TIDY_BEFORE_THROW();
#if defined(J9VM_OPT_VALHALLA_VALUE_TYPES)
			if (J9_IS_J9CLASS_VALUETYPE(badClass)) {
				currentThread->javaVM->internalVMFunctions->setCurrentExceptionNLSWithArgs(currentThread, J9NLS_VM_ERROR_BYTECODE_OBJECTREF_CANNOT_BE_VALUE_TYPE,
					J9VMCONSTANTPOOL_JAVALANGIDENTITYEXCEPTION, J9UTF8_LENGTH(className), J9UTF8_DATA(className));
			} else
#endif /* defined(J9VM_OPT_VALHALLA_VALUE_TYPES) */
			{
				Assert_CodertVM_true(J9_ARE_ALL_BITS_SET(currentThread->javaVM->extendedRuntimeFlags2, J9_EXTENDED_RUNTIME2_VALUE_BASED_EXCEPTION));
				currentThread->javaVM->internalVMFunctions->setCurrentExceptionNLSWithArgs(currentThread, J9NLS_VM_ERROR_BYTECODE_OBJECTREF_CANNOT_BE_VALUE_BASED,
					J9VMCONSTANTPOOL_JAVALANGVIRTUALMACHINEERROR, J9UTF8_LENGTH(className), J9UTF8_DATA(className));
			}
			addr = J9_JITHELPER_ACTION_THROW;
			break;
		}
#endif /* JAVA_SPEC_VERSION >= 16 */
#if defined(J9VM_OPT_CRIU_SUPPORT)
		case J9_OBJECT_MONITOR_CRIU_SINGLE_THREAD_MODE_THROW:
			addr = setCRIUSingleThreadModeExceptionFromJIT(currentThread, J9NLS_VM_CRIU_SINGLETHREADMODE_JVMCRIUEXCEPTION);
			break;
#endif /* defined(J9VM_OPT_CRIU_SUPPORT) */
		case J9_OBJECT_MONITOR_OOM:
			addr = setNativeOutOfMemoryErrorFromJIT(currentThread, J9NLS_VM_FAILED_TO_ALLOCATE_MONITOR);
			break;
#if JAVA_SPEC_VERSION >= 24
		case J9_OBJECT_MONITOR_YIELD_VIRTUAL:
			addr = J9_JITHELPER_ACTION_YIELD_AT_MONENT;
			break;
#endif /* JAVA_SPEC_VERSION >= 24 */
		default:
			Assert_CodertVM_unreachable();
		}
		goto done;
	} else {
		currentThread->javaVM->internalVMFunctions->objectMonitorEnterBlocking(currentThread);
		/* Do not check asyncs for synchronized block (non-method) entry, since we now have the monitor,
		 * but are not inside an exception range which will unlock the monitor if the checkAsync throws an exception.
		 */
		addr = restoreJITResolveFrame(currentThread, oldPC, forMethod, false);
	}
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitMethodMonitorEntry(J9VMThread *currentThread)
{
	return slow_jitMonitorEnterImpl(currentThread, true);
}

void* J9FASTCALL
old_fast_jitMethodMonitorEntry(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, syncObject, 1);
	void *slowPath = NULL;
	if (fast_jitMonitorEnterImpl(currentThread, syncObject, true)) {
		slowPath = (void*)old_slow_jitMethodMonitorEntry;
	}
	return slowPath;
}

void* J9FASTCALL
old_slow_jitMonitorEntry(J9VMThread *currentThread)
{
	return slow_jitMonitorEnterImpl(currentThread, false);
}

void* J9FASTCALL
old_fast_jitMonitorEntry(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, syncObject, 1);
	void *slowPath = NULL;
	if (fast_jitMonitorEnterImpl(currentThread, syncObject, false)) {
		slowPath = (void*)old_slow_jitMonitorEntry;
	}
	return slowPath;
}

static VMINLINE bool
fast_jitMonitorExitImpl(J9VMThread *currentThread, j9object_t syncObject, bool forMethod)
{
	bool slowPathRequired = true;
	currentThread->floatTemp1 = (void*)syncObject;
	currentThread->floatTemp2 = (void*)J9THREAD_WOULD_BLOCK;
	J9JavaVM *vm = currentThread->javaVM;
	/*  If there is a possibility that a hook will be reported from the helper, we need to build a frame */
	if (!J9_EVENT_IS_RESERVED(vm->hookInterface, J9HOOK_VM_MONITOR_CONTENDED_EXIT)) {
		IDATA monstatus = vm->internalVMFunctions->objectMonitorExit(currentThread, syncObject);
		if (0 == monstatus) {
			slowPathRequired = false;
		} else {
			currentThread->floatTemp2 = (void*)J9THREAD_ILLEGAL_MONITOR_STATE;
		}
	}
	return slowPathRequired;
}

static VMINLINE void*
slow_jitMonitorExitImpl(J9VMThread *currentThread, bool forMethod)
{
	SLOW_JIT_HELPER_PROLOGUE();
	j9object_t syncObject = (j9object_t)currentThread->floatTemp1;
	IDATA monstatus = (IDATA)(UDATA)currentThread->floatTemp2;
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	void *oldPC = NULL;
	void *addr = NULL;
	/* J9THREAD_WOULD_BLOCK indicates that the exit hook was reserved */
	if (J9THREAD_WOULD_BLOCK == monstatus) {
		oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		fixStackForSyntheticHandler(currentThread);
		monstatus = vmFuncs->objectMonitorExit(currentThread, syncObject);
	}
	if (0 != monstatus) {
		if (NULL == oldPC) {
			oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			fixStackForSyntheticHandler(currentThread);
		}
		addr = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALMONITORSTATEEXCEPTION, NULL);
		goto done;
	}
	if (NULL != oldPC) {
		addr = restoreJITResolveFrame(currentThread, oldPC);
	}
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitMethodMonitorExit(J9VMThread *currentThread)
{
	return slow_jitMonitorExitImpl(currentThread, true);
}

void* J9FASTCALL
old_slow_jitThrowIncompatibleReceiver(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(receiverClass, 1);
	DECLARE_JIT_CLASS_PARM(currentClass, 2);
	buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	TIDY_BEFORE_THROW();
	currentThread->javaVM->internalVMFunctions->setIllegalAccessErrorReceiverNotSameOrSubtypeOfCurrentClass(currentThread, receiverClass, currentClass);
	return J9_JITHELPER_ACTION_THROW;
}

void* J9FASTCALL
old_fast_jitMethodMonitorExit(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, syncObject, 1);
	void *slowPath = NULL;
	if (fast_jitMonitorExitImpl(currentThread, syncObject, true)) {
		slowPath = (void*)old_slow_jitMethodMonitorExit;
	}
	return slowPath;
}

void* J9FASTCALL
old_slow_jitMonitorExit(J9VMThread *currentThread)
{
	return slow_jitMonitorExitImpl(currentThread, false);
}

void* J9FASTCALL
old_fast_jitMonitorExit(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, syncObject, 1);
	void *slowPath = NULL;
	if (fast_jitMonitorExitImpl(currentThread, syncObject, false)) {
		slowPath = (void*)old_slow_jitMonitorExit;
	}
	return slowPath;
}

static VMINLINE void*
jitReportMethodEnterImpl(J9VMThread *currentThread, J9Method *method, j9object_t receiver, bool isStatic)
{
	SLOW_JIT_HELPER_PROLOGUE();
	J9JavaVM *vm = currentThread->javaVM;
	void *addr = NULL;
	bool hooked = J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_METHOD_ENTER);
	bool traced = VM_VMHelpers::methodBeingTraced(vm, method);
	if (hooked || traced) {
		void *oldPC = buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE, parmCount);
		j9object_t* receiverAddress = NULL;
		if (!isStatic) {
			/* The current value of the jitException slot is preserved in the resolve frame, so it can be safely overwritten */
			receiverAddress = &(currentThread->jitException);
			*receiverAddress = receiver;
		}
		if (traced) {
			UTSI_TRACEMETHODENTER_FROMVM(vm, currentThread, method, (void*)receiverAddress, 1);
		}
		if (hooked) {
			ALWAYS_TRIGGER_J9HOOK_VM_METHOD_ENTER(vm->hookInterface, currentThread, method, (void*)receiverAddress, 1);
		}
		addr = restoreJITResolveFrame(currentThread, oldPC, true, false);
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitReportMethodEnter(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	DECLARE_JIT_PARM(j9object_t, receiver, 2);
	return jitReportMethodEnterImpl(currentThread, method, receiver, false);
}

void* J9FASTCALL
old_slow_jitReportStaticMethodEnter(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	return jitReportMethodEnterImpl(currentThread, method, NULL, true);
}

void* J9FASTCALL
old_slow_jitReportMethodExit(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	DECLARE_JIT_PARM(void*, returnValueAddress, 2);
	J9JavaVM *vm = currentThread->javaVM;
	void *addr = NULL;
	bool hooked = J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_METHOD_RETURN);
	bool traced = VM_VMHelpers::methodBeingTraced(vm, method);
	if (hooked || traced) {
		void *oldPC = buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE, parmCount);
		if (traced) {
			UTSI_TRACEMETHODEXIT_FROMVM(vm, currentThread, method, NULL, returnValueAddress, 1);
		}
		if (hooked) {
			ALWAYS_TRIGGER_J9HOOK_VM_METHOD_RETURN(vm->hookInterface, currentThread, method, FALSE, returnValueAddress, 1);
		}
		addr = restoreJITResolveFrame(currentThread, oldPC, true, false);
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveClass(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
	J9Class *clazz = NULL;
//	remove until matching fix comes from TR
//	clazz = ((J9RAMClassRef*)ramConstantPool)[cpIndex].value;
	if (NULL == clazz) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		clazz = currentThread->javaVM->internalVMFunctions->resolveClassRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
	}
	JIT_RETURN_UDATA(clazz);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveClassFromStaticField(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
	J9RAMStaticFieldRef *ramStaticFieldRef = (J9RAMStaticFieldRef*)ramConstantPool + cpIndex;
	IDATA flagsAndClass = ramStaticFieldRef->flagsAndClass;
	UDATA valueOffset = ramStaticFieldRef->valueOffset;
	if (J9_UNEXPECTED(!VM_VMHelpers::staticFieldRefIsResolved(flagsAndClass, valueOffset))) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		J9RAMStaticFieldRef localRef;
		currentThread->javaVM->internalVMFunctions->resolveStaticFieldRefInto(currentThread, NULL, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE, NULL, &localRef);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		flagsAndClass = localRef.flagsAndClass;
	}
	JIT_RETURN_UDATA(((UDATA)flagsAndClass) << J9_REQUIRED_CLASS_SHIFT);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void J9FASTCALL
old_fast_jitResolvedFieldIsVolatile(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_PARM(UDATA, cpIndex, 2);
	DECLARE_JIT_PARM(UDATA, isStatic, 3);
	UDATA isVolatile = 0;
	if (isStatic) {
		if (J9_ARE_ANY_BITS_SET(((J9RAMStaticFieldRef*)ramConstantPool)[cpIndex].flagsAndClass, STATIC_FIELD_REF_BIT(J9StaticFieldRefVolatile))) {
			isVolatile = 1;
		}
	} else {
		if (J9_ARE_ANY_BITS_SET(((J9RAMFieldRef*)ramConstantPool)[cpIndex].flags, J9AccVolatile)) {
			isVolatile = 1;
		}
	}
	JIT_RETURN_UDATA(isVolatile);
}

static VMINLINE void*
old_slow_jitResolveFieldImpl(J9VMThread *currentThread, UDATA parmCount, J9ConstantPool *ramConstantPool, UDATA cpIndex, void *jitEIP, bool isSetter, bool direct)
{
	J9Method *method = NULL;
	UDATA resolveFlags = J9_RESOLVE_FLAG_RUNTIME_RESOLVE;
	J9JavaVM *vm = currentThread->javaVM;
	buildJITResolveFrameWithPC(currentThread, direct ? J9_SSF_JIT_RESOLVE : J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
	if (isSetter) {
		J9StackWalkState *walkState = currentThread->stackWalkState;
		walkState->walkThread = currentThread;
		walkState->skipCount = 0;
		walkState->flags = J9_STACKWALK_VISIBLE_ONLY | J9_STACKWALK_COUNT_SPECIFIED;
		walkState->maxFrames = 1;
		vm->walkStackFrames(currentThread, walkState);
		method = walkState->method;
		resolveFlags |= J9_RESOLVE_FLAG_FIELD_SETTER;
	}
	UDATA valueOffset = vm->internalVMFunctions->resolveInstanceFieldRef(currentThread, method, ramConstantPool, cpIndex, resolveFlags, NULL);
	void *addr = restoreJITResolveFrame(currentThread, jitEIP);
	if (NULL != addr) {
		goto done;
	}
	valueOffset += J9VMTHREAD_OBJECT_HEADER_SIZE(currentThread);
	JIT_RETURN_UDATA(valueOffset);
done:
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveField(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = old_slow_jitResolveFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, false, false);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveFieldSetter(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = old_slow_jitResolveFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, true, false);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveFieldDirect(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	void *jitEIP = currentThread->jitReturnAddress;
	void *addr = old_slow_jitResolveFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, false, true);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveFieldSetterDirect(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	void *jitEIP = currentThread->jitReturnAddress;
	void *addr = old_slow_jitResolveFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, true, true);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

static VMINLINE void*
old_slow_jitResolveStaticFieldImpl(J9VMThread *currentThread, UDATA parmCount, J9ConstantPool *ramConstantPool, UDATA cpIndex, void *jitEIP, bool isSetter, bool direct)
{
	J9JavaVM *vm = currentThread->javaVM;
	J9Method *method = NULL;
	UDATA resolveFlags = J9_RESOLVE_FLAG_RUNTIME_RESOLVE | J9_RESOLVE_FLAG_CHECK_CLINIT;
	buildJITResolveFrameWithPC(currentThread, direct ? J9_SSF_JIT_RESOLVE : J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
	if (isSetter) {
		J9StackWalkState *walkState = currentThread->stackWalkState;
		walkState->walkThread = currentThread;
		walkState->skipCount = 0;
		walkState->flags = J9_STACKWALK_VISIBLE_ONLY | J9_STACKWALK_COUNT_SPECIFIED;
		walkState->maxFrames = 1;
		vm->walkStackFrames(currentThread, walkState);
		method = walkState->method;
		resolveFlags |= J9_RESOLVE_FLAG_FIELD_SETTER;
	}
	UDATA field = (UDATA)vm->internalVMFunctions->resolveStaticFieldRef(currentThread, method, ramConstantPool, cpIndex, resolveFlags, NULL);
	if ((UDATA)-1 == field) {
		/* fetch result from floatTemp - stashed there in clinit case */
		J9RAMStaticFieldRef *fakeRef = (J9RAMStaticFieldRef*)&currentThread->floatTemp1;
		/* Resolve call indicated success - no need for extra putstatic checks, and the ref
		 * must contain valid resolved information.
		 */
		field = (UDATA)VM_VMHelpers::staticFieldAddressFromResolvedRef(fakeRef->flagsAndClass, fakeRef->valueOffset);
		/* Direct resolves do not patch the code cache, so do not need the clinit tag */
		if (!direct) {
			/* tag result as being from a clinit */
			field |= J9_RESOLVE_STATIC_FIELD_TAG_FROM_CLINIT;
		}
	}
	void *addr = restoreJITResolveFrame(currentThread, jitEIP);
	if (NULL == addr) {
		JIT_RETURN_UDATA(field);
	}
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveStaticField(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = old_slow_jitResolveStaticFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, false, false);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveStaticFieldSetter(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = old_slow_jitResolveStaticFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, true, false);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveStaticFieldDirect(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	void *jitEIP = currentThread->jitReturnAddress;
	void *addr = old_slow_jitResolveStaticFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, false, true);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveStaticFieldSetterDirect(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	void *jitEIP = currentThread->jitReturnAddress;
	void *addr = old_slow_jitResolveStaticFieldImpl(currentThread, parmCount, ramConstantPool, cpIndex, jitEIP, true, true);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveInterfaceMethod(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(UDATA*, indexAndLiteralsEA, 1);
	DECLARE_JIT_PARM(void*, jitEIP, 2);
	void *addr = NULL;
retry:
	J9ConstantPool *ramConstantPool = ((J9ConstantPool**)indexAndLiteralsEA)[0];
	UDATA cpIndex = indexAndLiteralsEA[1];
	J9RAMInterfaceMethodRef *ramMethodRef = (J9RAMInterfaceMethodRef*)ramConstantPool + cpIndex;
	J9Class* const interfaceClass = (J9Class*)ramMethodRef->interfaceClass;
	UDATA const methodIndexAndArgCount = ramMethodRef->methodIndexAndArgCount;
	if (!J9RAMINTERFACEMETHODREF_RESOLVED(interfaceClass, methodIndexAndArgCount)) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_INTERFACE_METHOD, parmCount, true, 0, jitEIP);
		currentThread->javaVM->internalVMFunctions->resolveInterfaceMethodRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	} else {
		omrthread_jit_write_protect_disable();
		indexAndLiteralsEA[2] = (UDATA)interfaceClass;
		UDATA methodIndex = methodIndexAndArgCount >> J9_ITABLE_INDEX_SHIFT;
		UDATA iTableOffset = 0;
		if (J9_ARE_ANY_BITS_SET(methodIndexAndArgCount, J9_ITABLE_INDEX_METHOD_INDEX)) {
			/* Direct method - methodIndex is an index into the method list of either Object or interfaceClass */
			J9Class *methodClass = interfaceClass;
			if (J9_ARE_ANY_BITS_SET(methodIndexAndArgCount, J9_ITABLE_INDEX_OBJECT)) {
				methodClass = J9VMJAVALANGOBJECT_OR_NULL(currentThread->javaVM);
			}
			iTableOffset = ((UDATA)(methodClass->ramMethods + methodIndex)) | J9_ITABLE_OFFSET_DIRECT;
		} else if (J9_ARE_ANY_BITS_SET(methodIndexAndArgCount, J9_ITABLE_INDEX_OBJECT)) {
			/* Virtual Object method  - methodIndex is the vTable offset */
			iTableOffset = methodIndex | J9_ITABLE_OFFSET_VIRTUAL;
		} else {
			/* Standard interface method - methodIndex is an index into the iTable */
			iTableOffset = (methodIndex * sizeof(UDATA)) + sizeof(J9ITable);
		}
		indexAndLiteralsEA[3] = iTableOffset;
		omrthread_jit_write_protect_enable();
		JIT_RETURN_UDATA(1);
	}
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveSpecialMethod(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(void*, jitEIP, 1);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 2);
	DECLARE_JIT_INT_PARM(cpIndex, 3);
	J9Method *method = NULL;
	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_SPECIAL_METHOD, parmCount, true, 0, jitEIP);
	/* If cpIndex has the J9_SPECIAL_SPLIT_TABLE_INDEX_FLAG bit set, it is an index into split table, otherwise it is an index in constant pool */
	if (J9_ARE_ANY_BITS_SET(cpIndex, J9_SPECIAL_SPLIT_TABLE_INDEX_FLAG)) {
		method = currentThread->javaVM->internalVMFunctions->resolveSpecialSplitMethodRef(currentThread, ramConstantPool, cpIndex & J9_SPLIT_TABLE_INDEX_MASK, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
	} else {
		method = currentThread->javaVM->internalVMFunctions->resolveSpecialMethodRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
	}
	void *addr = restoreJITResolveFrame(currentThread, jitEIP);
	if (NULL == addr) {
		JIT_RETURN_UDATA(method);
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveStaticMethod(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(void*, jitEIP, 1);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 2);
	DECLARE_JIT_INT_PARM(cpIndex, 3);
	UDATA method = 0;
	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_STATIC_METHOD, parmCount, true, 0, jitEIP);
	/* If cpIndex has the J9_STATIC_SPLIT_TABLE_INDEX_FLAG bit set, it is an index into split table, otherwise it is an index in constant pool */
	if (J9_ARE_ANY_BITS_SET(cpIndex, J9_STATIC_SPLIT_TABLE_INDEX_FLAG)) {
		method = (UDATA)currentThread->javaVM->internalVMFunctions->resolveStaticSplitMethodRef(currentThread, ramConstantPool, cpIndex & J9_SPLIT_TABLE_INDEX_MASK, J9_RESOLVE_FLAG_RUNTIME_RESOLVE | J9_RESOLVE_FLAG_CHECK_CLINIT);
	} else {
		method = (UDATA)currentThread->javaVM->internalVMFunctions->resolveStaticMethodRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE | J9_RESOLVE_FLAG_CHECK_CLINIT);
	}
	if ((UDATA)-1 == method) {
		/* fetch result from floatTemp - stashed there in clinit case */
		J9RAMStaticMethodRef *fakeRef = (J9RAMStaticMethodRef*)&currentThread->floatTemp1;
		method = (UDATA)fakeRef->method;
		/* tag result as being from a clinit */
		method |= J9_RESOLVE_STATIC_FIELD_TAG_FROM_CLINIT;
	}
	void *addr = restoreJITResolveFrame(currentThread, jitEIP);
	if (NULL == addr) {
		JIT_RETURN_UDATA(method);
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveVirtualMethod(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(UDATA*, indexAndLiteralsEA, 1);
	DECLARE_JIT_PARM(void*, jitEIP, 2);
	void *addr = NULL;
retry:
	J9ConstantPool *ramConstantPool = ((J9ConstantPool**)indexAndLiteralsEA)[0];
	UDATA cpIndex = indexAndLiteralsEA[1];
	J9RAMVirtualMethodRef *ramMethodRef = (J9RAMVirtualMethodRef*)ramConstantPool + cpIndex;
	UDATA vTableOffset = ramMethodRef->methodIndexAndArgCount >> 8;
	if (J9VTABLE_INITIAL_VIRTUAL_OFFSET == vTableOffset) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_VIRTUAL_METHOD, parmCount, true, 0, jitEIP);
		currentThread->javaVM->internalVMFunctions->resolveVirtualMethodRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE, NULL);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	}
	if (J9VTABLE_INVOKE_PRIVATE_OFFSET == vTableOffset) {
		UDATA method = ((UDATA)ramMethodRef->method) | J9_VTABLE_INDEX_DIRECT_METHOD_FLAG;
		JIT_RETURN_UDATA(method);
		goto done;
	}
	JIT_RETURN_UDATA(sizeof(J9Class) - vTableOffset);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveMethodType(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
retry:
	J9RAMMethodTypeRef *ramRef = (J9RAMMethodTypeRef*)ramConstantPool + cpIndex;
	j9object_t *methodTypePtr = &ramRef->type;
	if (NULL == *methodTypePtr) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		currentThread->javaVM->internalVMFunctions->resolveMethodTypeRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	}
	JIT_RETURN_UDATA(methodTypePtr);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveMethodHandle(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
retry:
	J9RAMMethodHandleRef *ramRef = (J9RAMMethodHandleRef*)ramConstantPool + cpIndex;
	j9object_t *methodHandlePtr = &ramRef->methodHandle;
	if (NULL == *methodHandlePtr) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		currentThread->javaVM->internalVMFunctions->resolveMethodHandleRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	}
	JIT_RETURN_UDATA(methodHandlePtr);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveInvokeDynamic(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(index, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
retry:
	J9Class *methodClass = ramConstantPool->ramClass;
	j9object_t *callsitePtr = methodClass->callSites + index;
	if (NULL == *callsitePtr) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		currentThread->javaVM->internalVMFunctions->resolveInvokeDynamic(currentThread, ramConstantPool, index, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	}
	JIT_RETURN_UDATA(callsitePtr);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveConstantDynamic(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
	J9JavaVM *vm = currentThread->javaVM;
retry:
	J9RAMConstantDynamicRef *ramCPEntry = (J9RAMConstantDynamicRef*)ramConstantPool + cpIndex;
	j9object_t *constantDynamicValue = &ramCPEntry->value;

	/* Check if the value is resolved, Void.Class exception represents a valid null reference */
	if ((NULL == *constantDynamicValue) && (ramCPEntry->exception != vm->voidReflectClass->classObject)) {
		/* If entry resolved to an exception previously, same exception will be set by resolution code */
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		vm->internalVMFunctions->resolveConstantDynamic(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	}
	JIT_RETURN_UDATA(constantDynamicValue);
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveHandleMethod(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9ConstantPool*, ramConstantPool, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	void *addr = NULL;
	J9JavaVM *vm = currentThread->javaVM;

#if defined(J9VM_OPT_OPENJDK_METHODHANDLE)
	J9RAMMethodRef *ramMethodRef = ((J9RAMMethodRef*)ramConstantPool) + cpIndex;
	UDATA invokeCacheIndex = ramMethodRef->methodIndexAndArgCount >> 8;
retry:
	j9object_t *invokeCacheArray = &((J9_CLASS_FROM_CP(ramConstantPool)->invokeCache)[invokeCacheIndex]);
	if (NULL == *invokeCacheArray) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		/* add new resolve code which calls sendResolveInvokeHandle -> MHN.linkMethod()
		 * store the memberName/appendix values in invokeCache[invokeCacheIndex]
		 */
		vm->internalVMFunctions->resolveOpenJDKInvokeHandle(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	}
	JIT_RETURN_UDATA(invokeCacheArray);
#else /* defined(J9VM_OPT_OPENJDK_METHODHANDLE) */
	j9object_t *methodTypePtr = NULL;
	j9object_t *methodTypeTablePtr = NULL;
	UDATA methodTypeIndex = 0;
	J9Class *methodClass = ramConstantPool->ramClass;
	J9ROMConstantPoolItem *romConstantPool = ramConstantPool->romConstantPool;
	J9ROMMethodRef *romMethodRef = (J9ROMMethodRef *)&(ramConstantPool->romConstantPool[cpIndex]);
	U_32 classRefCPIndex = romMethodRef->classRefCPIndex;
	J9RAMClassRef *ramClassRef = ((J9RAMClassRef *)ramConstantPool) + classRefCPIndex;
	J9Class *targetClass = ramClassRef->value;

	/* Ensure that the call site class ref is resolved */
	if (NULL == targetClass) {
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		targetClass = vm->internalVMFunctions->resolveClassRef(currentThread, ramConstantPool, classRefCPIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
	}

	if (J9VMJAVALANGINVOKEMETHODHANDLE_OR_NULL(vm) == targetClass) {
		/* MethodHandle */
		J9RAMMethodRef *ramRef = (J9RAMMethodRef *)&(ramConstantPool[cpIndex]);
		methodTypeIndex = ramRef->methodIndexAndArgCount >> 8;
		methodTypeTablePtr = methodClass->methodTypes;
	} else {
		/* VarHandle */
		J9ROMClass *romClass = methodClass->romClass;
		methodTypeIndex = VM_VMHelpers::lookupVarHandleMethodTypeCacheIndex(romClass, cpIndex);
		methodTypeTablePtr = methodClass->varHandleMethodTypes;
	}

retry:
	methodTypePtr = &(methodTypeTablePtr[methodTypeIndex]);

	if (NULL == *methodTypePtr) {
		/* Resolve the call site if unresolved */
		buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_DATA, parmCount, true, 0, jitEIP);
		vm->internalVMFunctions->resolveVirtualMethodRef(currentThread, ramConstantPool, cpIndex, J9_RESOLVE_FLAG_RUNTIME_RESOLVE, NULL);
		addr = restoreJITResolveFrame(currentThread, jitEIP);
		if (NULL != addr) {
			goto done;
		}
		goto retry;
	}

	JIT_RETURN_UDATA(methodTypePtr);
#endif /* defined(J9VM_OPT_OPENJDK_METHODHANDLE) */
done:
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitResolveFlattenableField(J9VMThread *currentThread)
{
	void *addr = NULL;

#if defined(J9VM_OPT_VALHALLA_FLATTENABLE_VALUE_TYPES)
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	DECLARE_JIT_INT_PARM(cpIndex, 2);
	DECLARE_JIT_INT_PARM(resolveType, 3);
	J9ConstantPool * const ramConstantPool = J9_CP_FROM_METHOD(method);
	J9RAMFieldRef * const ramFieldRef = ((J9RAMFieldRef *)ramConstantPool) + cpIndex;
	UDATA const flags = ramFieldRef->flags;
	UDATA const valueOffset = ramFieldRef->valueOffset;
	bool resolved = VM_VMHelpers::instanceFieldRefIsResolved(flags, valueOffset);
	if (resolved && (J9TR_FLAT_RESOLVE_PUTFIELD == resolveType)) {
		resolved = VM_VMHelpers::resolvedInstanceFieldRefIsPutResolved(flags, method, ramConstantPool);
	}
	if (!resolved) {
		UDATA resolveFlags = J9_RESOLVE_FLAG_RUNTIME_RESOLVE;
		switch(resolveType) {
		case J9TR_FLAT_RESOLVE_GETFIELD:
			break;
		case J9TR_FLAT_RESOLVE_PUTFIELD:
			resolveFlags |= J9_RESOLVE_FLAG_FIELD_SETTER;
			break;
		default:
			Assert_CodertVM_unreachable();
			break;
		}
		void *oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
		currentThread->javaVM->internalVMFunctions->resolveInstanceFieldRef(currentThread, method, ramConstantPool, cpIndex, resolveFlags, NULL);
		addr = restoreJITResolveFrame(currentThread, oldPC);
	}
	SLOW_JIT_HELPER_EPILOGUE();
#endif /* defined(J9VM_OPT_VALHALLA_FLATTENABLE_VALUE_TYPES) */

	return addr;
}

void* J9FASTCALL
old_slow_jitRetranslateCaller(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	DECLARE_JIT_PARM(void*, oldJITStartAddr, 2);
	J9JavaVM *vm = currentThread->javaVM;
	J9JITConfig *jitConfig = vm->jitConfig;
	void *oldPC = currentThread->jitReturnAddress;
	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_RUNTIME_HELPER, parmCount, false, 0, oldPC);
	UDATA oldState = currentThread->omrVMThread->vmState;
	currentThread->omrVMThread->vmState = J9VMSTATE_JIT;
	UDATA jitStartPC = jitConfig->entryPoint(jitConfig, currentThread, method, oldJITStartAddr);
	currentThread->omrVMThread->vmState = oldState;
	void *addr = restoreJITResolveFrame(currentThread, oldPC, true, false);
	if (NULL == addr) {
		JIT_RETURN_UDATA(jitStartPC);
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitRetranslateCallerWithPreparation(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	DECLARE_JIT_PARM(void*, oldJITStartAddr, 2);
	DECLARE_JIT_PARM(UDATA, reason, 3);
	J9JavaVM *vm = currentThread->javaVM;
	J9JITConfig *jitConfig = vm->jitConfig;
	void *oldPC = currentThread->jitReturnAddress;
	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_RUNTIME_HELPER, parmCount, false, 0, oldPC);
	UDATA oldState = currentThread->omrVMThread->vmState;
	currentThread->omrVMThread->vmState = J9VMSTATE_JIT;
	UDATA jitStartPC = jitConfig->retranslateWithPreparation(jitConfig, currentThread, method, oldJITStartAddr, reason);
	currentThread->omrVMThread->vmState = oldState;
	void *addr = restoreJITResolveFrame(currentThread, oldPC, true, false);
	if (NULL == addr) {
		JIT_RETURN_UDATA(jitStartPC);
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

J9_EXTERN_BUILDER_SYMBOL(jitDecompileAtCurrentPC);

void* J9FASTCALL
old_slow_jitRetranslateMethod(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9Method*, method, 1);
	DECLARE_JIT_PARM(void*, oldJITStartAddr, 2);
	DECLARE_JIT_PARM(void*, jitEIP, 3);
	J9JavaVM *vm = currentThread->javaVM;
	J9JITConfig *jitConfig = vm->jitConfig;
	UDATA jitStartPC = 0;

	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE_RECOMPILATION, parmCount, true, 0, jitEIP);

	/* The return address of the recompiled method is either:
	 * 	No special handling required
	 * 		- a jitted caller
	 * 		- the interpreter (something in the return table)
	 * Special handling required
	 *		- a decompile point
	 */
	bool callerAlreadyBeingDecompiled  = false;
	if (NULL == jitGetExceptionTableFromPC(currentThread, (UDATA)jitEIP)) {
		U_8 **returnTable = (U_8**)jitConfig->i2jReturnTable;
		callerAlreadyBeingDecompiled = true;
		for (UDATA i = 0; i < J9SW_JIT_RETURN_TABLE_SIZE; ++i) {
			if (jitEIP == returnTable[i]) {
				callerAlreadyBeingDecompiled = false;
				break;
			}
		}
	}
	if (callerAlreadyBeingDecompiled) {
		/* The caller is already being decompiled.  The only way this can occur is if:
		 *
		 *	- the caller is jitted
		 *	- caller invokes a non-compiled method
		 *	- interpreter submits target for compilation
		 *	- during compilation, a decomp is added for the caller
		 *	- target is successfully compiled
		 *	- decomp record is updated such that the savedPCAddres points to where the jitted method would save the RA
		 *	- interp transitions to the newly-compiled method with the return address pointing to a decompilation
		 *
		 * The decompilation assumes that the target method would run (so it's jitDecompileOnReturn*).
		 * This is incorrect at this point, since we won't run the target method, so change the decompile to
		 * jitDecompileAtCurrentPC (to re-run the invoke after decomp), and change the savedPCAddress in the decomp
		 * record to point to the returnAddress field of the recently pushed resolve frame to keep the stack walker happy.
		 */
		J9SFJITResolveFrame *resolveFrame = (J9SFJITResolveFrame*)currentThread->sp;
		U_8 **savedPCAddress = (U_8**)&resolveFrame->returnAddress;
		*savedPCAddress = (U_8*)J9_BUILDER_SYMBOL(jitDecompileAtCurrentPC);
		currentThread->decompilationStack->pcAddress = savedPCAddress;
	} else {
		UDATA oldState = currentThread->omrVMThread->vmState;
		currentThread->omrVMThread->vmState = J9VMSTATE_JIT;
		jitStartPC = jitConfig->entryPoint(jitConfig, currentThread, method, oldJITStartAddr);
		currentThread->omrVMThread->vmState = oldState;
	}
	void *addr = restoreJITResolveFrame(currentThread, jitEIP, true, false);
	if (NULL == addr) {
		JIT_RETURN_UDATA(jitStartPC);
	}
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitThrowCurrentException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return J9_JITHELPER_ACTION_THROW;
}

void* J9FASTCALL
old_slow_jitThrowException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, exception, 1);
	VM_VMHelpers::setExceptionPending(currentThread, exception);
	buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE, parmCount);
	return J9_JITHELPER_ACTION_THROW;
}

void* J9FASTCALL
old_slow_jitThrowUnreportedException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, exception, 1);
	buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE, parmCount);
	fixStackForSyntheticHandler(currentThread);
	VM_VMHelpers::setExceptionPending(currentThread, exception, false);
	return J9_JITHELPER_ACTION_THROW;
}

void* J9FASTCALL
old_slow_jitThrowAbstractMethodError(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGABSTRACTMETHODERROR, NULL);
}

void* J9FASTCALL
old_slow_jitThrowArithmeticException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionNLSFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARITHMETICEXCEPTION, J9NLS_VM_DIVIDE_BY_ZERO);
}

void* J9FASTCALL
old_slow_jitThrowArrayIndexOutOfBounds(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitThrowIdentityException(J9VMThread *currentThread)
{
	void *exception = NULL;
#if defined(J9VM_OPT_VALHALLA_VALUE_TYPES)
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	exception = setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGIDENTITYEXCEPTION, NULL);
#else /* defined(J9VM_OPT_VALHALLA_VALUE_TYPES) */
	exception = (void *)-1;
#endif /* defined(J9VM_OPT_VALHALLA_VALUE_TYPES) */
	return exception;
}

void* J9FASTCALL
impl_jitReferenceArrayCopy(J9VMThread *currentThread, UDATA lengthInBytes)
{
	JIT_HELPER_PROLOGUE();
	void* exception = NULL;
	if (-1 != currentThread->javaVM->memoryManagerFunctions->referenceArrayCopy(
		currentThread,
		(J9IndexableObject*)currentThread->floatTemp1,
		(J9IndexableObject*)currentThread->floatTemp2,
		(fj9object_t*)currentThread->floatTemp3,
		(fj9object_t*)currentThread->floatTemp4,
		(I_32)(lengthInBytes / J9VMTHREAD_REFERENCE_SIZE(currentThread))
	)) {
		exception = (void*)-1;
	}
	return exception;
}

void* J9FASTCALL
old_slow_jitThrowArrayStoreException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitThrowArrayStoreExceptionWithIP(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(void*, jitEIP, 1);
	buildJITResolveFrameWithPC(currentThread, J9_SSF_JIT_RESOLVE, parmCount, true, 0, jitEIP);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitThrowExceptionInInitializerError(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGEXCEPTIONININITIALIZERERROR, NULL);
}

void* J9FASTCALL
old_slow_jitThrowIllegalAccessError(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALACCESSERROR, NULL);
}

void* J9FASTCALL
old_slow_jitThrowIncompatibleClassChangeError(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGINCOMPATIBLECLASSCHANGEERROR, NULL);
}

void* J9FASTCALL
old_slow_jitThrowInstantiationException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGINSTANTIATIONEXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitThrowNullPointerException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitThrowWrongMethodTypeException(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	buildJITResolveFrameForRuntimeCheck(currentThread);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGINVOKEWRONGMETHODTYPEEXCEPTION, NULL);
}

void* J9FASTCALL
old_slow_jitTypeCheckArrayStoreWithNullCheck(J9VMThread *currentThread)
{
	SLOW_JIT_HELPER_PROLOGUE();
	buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	return setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION, NULL);
}

static VMINLINE bool
fast_jitTypeCheckArrayStoreImpl(J9VMThread *currentThread, j9object_t destinationObject, j9object_t objectBeingStored)
{
	bool slowPathRequired = false;
	if ((NULL != destinationObject) && (NULL != objectBeingStored)) {
		J9Class *objectClass = J9OBJECT_CLAZZ(currentThread, objectBeingStored);
		J9Class *componentType = ((J9ArrayClass*)J9OBJECT_CLAZZ(currentThread, destinationObject))->componentType;
		/* Quick check -- is this a store of a C into a C[]? */
		if (objectClass != componentType) {
			/* Quick check -- is this a store of a C into a java.lang.Object[]? */
			if (0 != VM_VMHelpers::getClassDepth(componentType)) {
				if (!VM_VMHelpers::inlineCheckCast(objectClass, componentType)) {
					slowPathRequired = true;
				}
			}
		}
	}
	return slowPathRequired;
}

void* J9FASTCALL
old_fast_jitTypeCheckArrayStoreWithNullCheck(J9VMThread *currentThread)
{
	void *slowPath = NULL;
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(j9object_t, destinationObject, 1);
	DECLARE_JIT_PARM(j9object_t, objectBeingStored, 2);
	if (fast_jitTypeCheckArrayStoreImpl(currentThread, destinationObject, objectBeingStored)) {
		slowPath = (void*)old_slow_jitTypeCheckArrayStoreWithNullCheck;
	}
	return slowPath;
}

void J9FASTCALL
old_fast_jitAcmpeqHelper(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(j9object_t, lhs, 1);
	DECLARE_JIT_PARM(j9object_t, rhs, 2);

	JIT_RETURN_UDATA(currentThread->javaVM->internalVMFunctions->valueTypeCapableAcmp(currentThread, lhs, rhs));
}

void J9FASTCALL
old_fast_jitAcmpneHelper(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(j9object_t, lhs, 1);
	DECLARE_JIT_PARM(j9object_t, rhs, 2);

	JIT_RETURN_UDATA(!currentThread->javaVM->internalVMFunctions->valueTypeCapableAcmp(currentThread, lhs, rhs));
}

void* J9FASTCALL
old_fast_jitTypeCheckArrayStore(J9VMThread *currentThread)
{
	/* Not omitting NULL check */
	return old_fast_jitTypeCheckArrayStoreWithNullCheck(currentThread);
}

void* J9FASTCALL
old_slow_jitTypeCheckArrayStore(J9VMThread *currentThread)
{
	return old_slow_jitTypeCheckArrayStoreWithNullCheck(currentThread);
}

void J9FASTCALL
fast_jitWriteBarrierBatchStore(J9VMThread *currentThread, j9object_t destinationObject)
{
	currentThread->javaVM->memoryManagerFunctions->J9WriteBarrierBatch(currentThread, destinationObject);
}

void J9FASTCALL
old_fast_jitWriteBarrierBatchStore(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, destinationObject, 1);
	fast_jitWriteBarrierBatchStore(currentThread, destinationObject);
}

void J9FASTCALL
fast_jitWriteBarrierBatchStoreWithRange(J9VMThread *currentThread, j9object_t destinationObject, UDATA destinationPtr, UDATA range)
{
	currentThread->javaVM->memoryManagerFunctions->J9WriteBarrierBatch(currentThread, destinationObject);
}

void J9FASTCALL
old_fast_jitWriteBarrierBatchStoreWithRange(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(j9object_t, destinationObject, 1);
	DECLARE_JIT_PARM(UDATA, destinationPtr, 2);
	DECLARE_JIT_PARM(UDATA, range, 3);
	fast_jitWriteBarrierBatchStoreWithRange(currentThread, destinationObject, destinationPtr, range);
}

void J9FASTCALL
fast_jitWriteBarrierJ9ClassBatchStore(J9VMThread *currentThread, J9Class *destinationClass)
{
	currentThread->javaVM->memoryManagerFunctions->J9WriteBarrierClassBatch(currentThread, destinationClass);
}

void J9FASTCALL
old_fast_jitWriteBarrierJ9ClassBatchStore(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_CLASS_PARM(destinationClass, 1);
	fast_jitWriteBarrierJ9ClassBatchStore(currentThread, destinationClass);
}

void J9FASTCALL
fast_jitWriteBarrierJ9ClassStore(J9VMThread *currentThread, J9Class *destinationClass, j9object_t objectBeingStored)
{
	currentThread->javaVM->memoryManagerFunctions->J9WriteBarrierPostClass(currentThread, destinationClass, objectBeingStored);
}

void J9FASTCALL
old_fast_jitWriteBarrierJ9ClassStore(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_CLASS_PARM(destinationClass, 1);
	DECLARE_JIT_PARM(j9object_t, objectBeingStored, 2);
	fast_jitWriteBarrierJ9ClassStore(currentThread, destinationClass, objectBeingStored);
}

void J9FASTCALL
fast_jitWriteBarrierStore(J9VMThread *currentThread, j9object_t destinationObject, j9object_t objectBeingStored)
{
	currentThread->javaVM->memoryManagerFunctions->J9WriteBarrierPost(currentThread, destinationObject, objectBeingStored);
}

void J9FASTCALL
old_fast_jitWriteBarrierStore(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(j9object_t, destinationObject, 1);
	DECLARE_JIT_PARM(j9object_t, objectBeingStored, 2);
	fast_jitWriteBarrierStore(currentThread, destinationObject, objectBeingStored);
}

void J9FASTCALL
fast_jitWriteBarrierStoreGenerational(J9VMThread *currentThread, j9object_t destinationObject, j9object_t objectBeingStored)
{
	MM_ObjectAccessBarrierAPI::internalPostObjectStoreGenerationalNoValueCheck(currentThread, destinationObject);
}

void J9FASTCALL
old_fast_jitWriteBarrierStoreGenerational(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(j9object_t, destinationObject, 1);
	DECLARE_JIT_PARM(j9object_t, objectBeingStored, 2);
	fast_jitWriteBarrierStoreGenerational(currentThread, destinationObject, objectBeingStored);
}

void J9FASTCALL
fast_jitWriteBarrierStoreGenerationalAndConcurrentMark(J9VMThread *currentThread, j9object_t destinationObject, j9object_t objectBeingStored)
{
	MM_ObjectAccessBarrierAPI::internalPostObjectStoreCardTableAndGenerational(currentThread, destinationObject, objectBeingStored);
}

void J9FASTCALL
old_fast_jitWriteBarrierStoreGenerationalAndConcurrentMark(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(j9object_t, destinationObject, 1);
	DECLARE_JIT_PARM(j9object_t, objectBeingStored, 2);
	fast_jitWriteBarrierStoreGenerationalAndConcurrentMark(currentThread, destinationObject, objectBeingStored);
}

void J9FASTCALL
fast_jitWriteBarrierClassStoreMetronome(J9VMThread *currentThread, j9object_t destinationClassObject, J9Object** destinationAddress, j9object_t objectBeingStored)
{
	currentThread->javaVM->memoryManagerFunctions->J9WriteBarrierPreClass(currentThread, destinationClassObject, destinationAddress, objectBeingStored);
}

void J9FASTCALL
old_fast_jitWriteBarrierClassStoreMetronome(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(j9object_t, destinationClassObject, 1);
	DECLARE_JIT_PARM(J9Object**, destinationAddress, 2);
	DECLARE_JIT_PARM(j9object_t, objectBeingStored, 3);
	fast_jitWriteBarrierClassStoreMetronome(currentThread, destinationClassObject, destinationAddress, objectBeingStored);
}

void J9FASTCALL
fast_jitWriteBarrierStoreMetronome(J9VMThread *currentThread, j9object_t destinationObject, fj9object_t* destinationAddress, j9object_t objectBeingStored)
{
	currentThread->javaVM->memoryManagerFunctions->J9WriteBarrierPre(currentThread, destinationObject, destinationAddress, objectBeingStored);
}

void J9FASTCALL
old_fast_jitWriteBarrierStoreMetronome(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(j9object_t, destinationObject, 1);
	DECLARE_JIT_PARM(fj9object_t*, destinationAddress, 2);
	DECLARE_JIT_PARM(j9object_t, objectBeingStored, 3);
	fast_jitWriteBarrierStoreMetronome(currentThread, destinationObject, destinationAddress, objectBeingStored);
}

void J9FASTCALL
old_slow_jitCallJitAddPicToPatchOnClassUnload(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(void*, classPointer, 1);
	DECLARE_JIT_PARM(void*, addressToBePatched, 2);
	jitAddPicToPatchOnClassUnload(classPointer, addressToBePatched);
	SLOW_JIT_HELPER_EPILOGUE();
}

typedef void (*twoVoidFunc)(void*, void*);

void J9FASTCALL
old_slow_jitCallCFunction(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(twoVoidFunc, functionPointer, 1);
	DECLARE_JIT_PARM(void*, argumentPointer, 2);
	DECLARE_JIT_PARM(void*, returnValuePointer, 3);
	functionPointer(argumentPointer, returnValuePointer);
	SLOW_JIT_HELPER_EPILOGUE();
}

void
fast_jitPreJNICallOffloadCheck(J9VMThread *currentThread)
{
#if defined(J9VM_OPT_JAVA_OFFLOAD_SUPPORT)
	OLD_JIT_HELPER_PROLOGUE(0);
#if defined(J9VM_PORT_ZOS_CEEHDLRSUPPORT)
	J9JavaVM *vm = currentThread->javaVM;
	if (J9_ARE_ANY_BITS_SET(vm->sigFlags, J9_SIG_ZOS_CEEHDLR)) {
		J9VMEntryLocalStorage *els = currentThread->entryLocalStorage;
		/* Save the old vmstate then set it to 'J9VMSTATE_JNI_FROM_JIT' and store the FPRs 8-15 */
		els->calloutVMState = setVMState(currentThread, J9VMSTATE_JNI_FROM_JIT);
	}
#endif /* J9VM_PORT_ZOS_CEEHDLRSUPPORT */
	VM_VMHelpers::beforeJNICall(currentThread);
#endif /* J9VM_OPT_JAVA_OFFLOAD_SUPPORT */
}

void
fast_jitPostJNICallOffloadCheck(J9VMThread *currentThread)
{
#if defined(J9VM_OPT_JAVA_OFFLOAD_SUPPORT)
	OLD_JIT_HELPER_PROLOGUE(0);
#if defined(J9VM_PORT_ZOS_CEEHDLRSUPPORT)
	J9JavaVM *vm = currentThread->javaVM;
	J9VMEntryLocalStorage *els = currentThread->entryLocalStorage;
	if (J9_ARE_ANY_BITS_SET(vm->sigFlags, J9_SIG_ZOS_CEEHDLR)) {
		setVMState(currentThread, els->calloutVMState);
		/* verify that the native didn't resume execution after an LE condition and illegally return to Java */
		if (J9_ARE_ANY_BITS_SET(currentThread->privateFlags, J9_PRIVATE_FLAGS_STACKS_OUT_OF_SYNC)) {
			vm->internalVMFunctions->javaAndCStacksMustBeInSync(currentThread, TRUE);
		}
	}
#endif /* J9VM_PORT_ZOS_CEEHDLRSUPPORT */
	VM_VMHelpers::afterJNICall(currentThread);
#endif /* J9VM_OPT_JAVA_OFFLOAD_SUPPORT */
}

void J9FASTCALL
old_fast_jitObjectHashCode(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(j9object_t, object, 1);
	J9JavaVM *vm = currentThread->javaVM;
	JIT_RETURN_UDATA((IDATA)vm->memoryManagerFunctions->j9gc_objaccess_getObjectHashCode(vm, (J9Object*)object));
}

static VMINLINE void*
old_slow_jitInduceOSRAtCurrentPCImpl(J9VMThread *currentThread, void *oldPC)
{
	induceOSROnCurrentThread(currentThread);
	/* If the OSR was successful, a decompilation will have been added to the resolve frame */
	J9SFJITResolveFrame *resolveFrame = (J9SFJITResolveFrame*)currentThread->sp;
	void *newPC = resolveFrame->returnAddress;
	if (samePCs(oldPC, newPC)) {
		/* If execution reaches this point (no decompilation added), then there was an out of memory error */
		newPC = setNativeOutOfMemoryErrorFromJIT(currentThread, 0, 0);
	} else {
		newPC = JIT_RUN_ON_JAVA_STACK(newPC);
	}
	return newPC;
}

void* J9FASTCALL
old_slow_jitInduceOSRAtCurrentPCAndRecompile(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	void *oldPC = buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE_INDUCE_OSR, parmCount);

	J9StackWalkState walkState;
	/* The stack currently has a resolve frame on top, followed by the JIT frame we
	 * wish to OSR.  Walk the stack down to the JIT frame to gather the data required
	 * for decompilation.
	 */
	J9JavaVM *vm = currentThread->javaVM;
	J9JITConfig *jitConfig = vm->jitConfig;
	walkState.walkThread = currentThread;
	walkState.maxFrames = 2;
	walkState.flags = J9_STACKWALK_SKIP_INLINES | J9_STACKWALK_COUNT_SPECIFIED;
	vm->walkStackFrames(currentThread, &walkState);
	J9Method *method = walkState.method;
	J9JITExceptionTable *metaData = walkState.jitInfo;
	void *oldJITStartAddr = (void *) metaData->startPC;
	jitConfig->retranslateWithPreparationForMethodRedefinition(jitConfig, currentThread, method, oldJITStartAddr);

	return old_slow_jitInduceOSRAtCurrentPCImpl(currentThread, oldPC);
}

void* J9FASTCALL
old_slow_jitInduceOSRAtCurrentPC(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(0);
	void *oldPC = buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE_INDUCE_OSR, parmCount);
	return old_slow_jitInduceOSRAtCurrentPCImpl(currentThread, oldPC);
}

void J9FASTCALL
old_slow_jitInterpretNewInstanceMethod(J9VMThread *currentThread)
{
	/* JIT has passed two parameters, but only the first one matters to this call.
	 * The parmCount has to be 1 in order for JIT_PARM_IN_MEMORY to function correctly
	 * in the register case.
	 */
	OLD_JIT_HELPER_PROLOGUE(1);
	J9JavaVM *vm = currentThread->javaVM;
#if defined(J9SW_NEEDS_JIT_2_INTERP_CALLEE_ARG_POP)
	/* Arguments are on stack in invoke order:
	 *
	 * sp[0] = callerClass
	 * sp[1] = objectClass
	 *
	 * Going to the interpreter, we need to discard callerClass in order to run the static method
	 * J9VMInternals.newInstanceImpl(Class objectClass), so the stack needs to look like:
	 *
	 * sp[0] = objectClass
	 */
	currentThread->sp += 1;
#else /* J9SW_NEEDS_JIT_2_INTERP_CALLEE_ARG_POP */
	/* Arguments are in registers.
	 *
	 * Copy the objectClassObject argument to the stack for the static interpreter dispatch.
	 */
	JIT_PARM_IN_MEMORY(1) = JIT_DIRECT_CALL_PARM(1);
#endif /* J9SW_NEEDS_JIT_2_INTERP_CALLEE_ARG_POP */
	currentThread->tempSlot = (UDATA)J9_BUILDER_SYMBOL(icallVMprJavaSendStatic1);
	jitRegisters->JIT_J2I_METHOD_REGISTER = (UDATA)J9VMJAVALANGJ9VMINTERNALS_NEWINSTANCEIMPL_METHOD(vm);
}

void* J9FASTCALL
old_slow_jitNewInstanceImplAccessCheck(J9VMThread *currentThread)
{
	void *addr = NULL;
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(j9object_t, thisClassObject, 1);
	DECLARE_JIT_PARM(j9object_t, callerClassObject, 2);
	DECLARE_JIT_PARM(J9Method*, defaultConstructor, 3);
	J9Class *thisClass = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, thisClassObject);
	J9Class *callerClass = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, callerClassObject);
 	J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(defaultConstructor);
 	J9InternalVMFunctions *vmFuncs = currentThread->javaVM->internalVMFunctions;
	void *oldPC = buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE_RUNTIME_HELPER, parmCount);
	IDATA checkResult = vmFuncs->checkVisibility(currentThread, callerClass, thisClass, thisClass->romClass->modifiers, J9_LOOK_REFLECT_CALL);
	thisClass = VM_VMHelpers::currentClass(thisClass);
	callerClass = VM_VMHelpers::currentClass(callerClass);
	if (checkResult < J9_VISIBILITY_ALLOWED) {
illegalAccess:
		if (VM_VMHelpers::immediateAsyncPending(currentThread)) {
			goto done;
		}
		if (VM_VMHelpers::exceptionPending(currentThread)) {
			goto done;
		}
 		J9UTF8 *classNameUTF = J9ROMCLASS_CLASSNAME(thisClass->romClass);
		J9UTF8 *nameUTF = J9ROMMETHOD_NAME(romMethod);
		J9UTF8 *sigUTF = J9ROMMETHOD_SIGNATURE(romMethod);
		j9object_t detailMessage = currentThread->javaVM->internalVMFunctions->catUtfToString4(
				currentThread,
				J9UTF8_DATA(classNameUTF),
				J9UTF8_LENGTH(classNameUTF),
				(U_8*)".",
				1,
				J9UTF8_DATA(nameUTF),
				J9UTF8_LENGTH(nameUTF),
				J9UTF8_DATA(sigUTF),
				J9UTF8_LENGTH(sigUTF));
		if (NULL != detailMessage) {
			setCurrentExceptionFromJIT(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALACCESSEXCEPTION, detailMessage);
		}
		goto done;
	}

	/* See if the method is visible - don't use checkVisibility here because newInstance
	 * has different rules for protected method visibility.
	 */
	if (!J9_ARE_ANY_BITS_SET(romMethod->modifiers, J9AccPublic)) {
		if (J9_ARE_ANY_BITS_SET(romMethod->modifiers, J9AccPrivate)) {
			/* Private methods are visible only within their declaring class
			 * unless the two classes are in the same nest (JDK11 and beyond).
			 */
#if JAVA_SPEC_VERSION >= 11
			J9Class *thisClassNestHost = thisClass->nestHost;
			J9Class *callerClassNestHost = NULL;
			if (NULL == thisClassNestHost) {
				if (J9_VISIBILITY_ALLOWED != vmFuncs->loadAndVerifyNestHost(currentThread, thisClass, 0, &thisClassNestHost)) {
					goto illegalAccess;
				}
			}
			callerClassNestHost = callerClass->nestHost;
			if (NULL == callerClassNestHost) {
				if (J9_VISIBILITY_ALLOWED != vmFuncs->loadAndVerifyNestHost(currentThread, callerClass, 0, &callerClassNestHost)) {
					goto illegalAccess;
				}
			}
			if (thisClassNestHost != callerClassNestHost)
#endif /* JAVA_SPEC_VERSION >= 11 */
			{
				if (thisClass != callerClass) {
					goto illegalAccess;
				}
			}
		} else {
			/* Protected and default are treated the same when called from newInstance() */
			if (thisClass->packageID != callerClass->packageID) {
				goto illegalAccess;
			}
		}
	}
done:
	addr = restoreJITResolveFrame(currentThread, oldPC);
	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void J9FASTCALL
old_slow_jitTranslateNewInstanceMethod(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	j9object_t objectClassObject = (j9object_t)JIT_DIRECT_CALL_PARM(1);
	j9object_t callerClassObject = (j9object_t)JIT_DIRECT_CALL_PARM(2);
	J9Class *objectClass = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, objectClassObject);
	J9Class *callerClass = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, callerClassObject);
redo:
	UDATA preCount = objectClass->newInstanceCount;
	if ((IDATA)preCount >= 0) {
		UDATA postCount = preCount - 1;
		UDATA result = VM_AtomicSupport::lockCompareExchange((UDATA*)&objectClass->newInstanceCount, preCount, postCount);
		if (result != preCount) {
			goto redo;
		}
		if ((IDATA)postCount < 0) {
			buildJITResolveFrame(currentThread, J9_SSF_JIT_RESOLVE, parmCount);
			PUSH_OBJECT_IN_SPECIAL_FRAME(currentThread, objectClassObject);
			PUSH_OBJECT_IN_SPECIAL_FRAME(currentThread, callerClassObject);
			UDATA oldState = currentThread->omrVMThread->vmState;
			currentThread->omrVMThread->vmState = J9VMSTATE_JIT;
			J9JavaVM *vm = currentThread->javaVM;
			J9JITConfig *jitConfig = vm->jitConfig;
			jitConfig->entryPointForNewInstance(jitConfig, currentThread, objectClass);
			currentThread->omrVMThread->vmState = oldState;
			callerClassObject = POP_OBJECT_IN_SPECIAL_FRAME(currentThread);
			objectClassObject = POP_OBJECT_IN_SPECIAL_FRAME(currentThread);
			/* The newInstance prototype optimization is disabled under FSD, so there is no
			 * possibility of decompilation occurring.  If it is ever enabled, something
			 * will need to be done here.
			 */
			restoreJITResolveFrame(currentThread, NULL, false, false);
			SET_JIT_DIRECT_CALL_PARM(1, objectClassObject);
			SET_JIT_DIRECT_CALL_PARM(2, callerClassObject);
		}
	}
	void *address = (void*)objectClass->romableAotITable;
	/* To handle async compilation - if the newInstance slot has not been updated, run the interp version */
	if (J9_BUILDER_SYMBOL(jitTranslateNewInstanceMethod) == address) {
		address = J9_BUILDER_SYMBOL(jitInterpretNewInstanceMethod);
	}
	currentThread->tempSlot = (UDATA)address;
	SLOW_JIT_HELPER_EPILOGUE();
}

#if !defined(J9VM_ENV_DATA64)

void J9FASTCALL
old_fast_jitVolatileReadLong(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(U_64*, address, 1);
	U_64 value = longVolatileRead(currentThread, address);
	JIT_RETURN_U64(value);
}

void J9FASTCALL
old_fast_jitVolatileWriteLong(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(U_64*, address, 1);
	DECLARE_JIT_U64_PARM(value, 2);
	longVolatileWrite(currentThread, address, (U_64*)&value);
}

void J9FASTCALL
old_fast_jitVolatileReadDouble(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(U_64*, address, 1);
	U_64 value = longVolatileRead(currentThread, address);
	jitRegisters->fpr[0] = value;
}

void J9FASTCALL
old_fast_jitVolatileWriteDouble(J9VMThread *currentThread)
{
	OLD_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(U_64*, address, 1);
	longVolatileWrite(currentThread, address, &(jitRegisters->fpr[0]));
}

#endif /* !J9VM_ENV_DATA64 */

#if defined(J9SW_NEEDS_JIT_2_INTERP_THUNKS)

void J9FASTCALL
old_slow_icallVMprJavaSendPatchupVirtual(J9VMThread *currentThread)
{
	UDATA const interfaceVTableIndex = currentThread->tempSlot;
	j9object_t const receiver = (j9object_t)currentThread->returnValue2;
	J9JavaVM *vm = currentThread->javaVM;
	J9JITConfig *jitConfig = vm->jitConfig;
	void *jitReturnAddress = currentThread->jitReturnAddress;
	UDATA jitVTableOffset = VM_JITInterface::jitVTableIndex(jitReturnAddress, interfaceVTableIndex);
	J9Class *clazz = J9OBJECT_CLAZZ(currentThread, receiver);
	UDATA interpVTableOffset = sizeof(J9Class) - jitVTableOffset;
	J9Method *method = *(J9Method**)((UDATA)clazz + interpVTableOffset);
	UDATA thunk = 0;
#if defined(J9VM_OPT_OPENJDK_METHODHANDLE)
	if (J9_BCLOOP_SEND_TARGET_METHODHANDLE_INVOKEBASIC == J9_BCLOOP_DECODE_SEND_TARGET(method->methodRunAddress)) {
		/* Because invokeBasic() is signature-polymorphic, the signature from the ROM method is irrelevant.
		 * We need the J2I thunk that corresponds to the signature that the JIT was using at the call site.
		 * Since this varies depending on the call site, we can't replace the VFT entry with the J2I thunk.
		 */
		J9JITInvokeBasicCallSite *site = jitGetInvokeBasicCallSiteFromPC(currentThread, (UDATA)jitReturnAddress);
		thunk = (UDATA)site->j2iThunk;
	}
	else
#endif /* defined(J9VM_OPT_OPENJDK_METHODHANDLE) */
	{
		J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(method);
		J9ROMNameAndSignature *nas = &romMethod->nameAndSignature;
		thunk = (UDATA)jitConfig->thunkLookUpNameAndSig(jitConfig, nas);
		UDATA const patchup = (UDATA)jitConfig->patchupVirtual;
		UDATA *jitVTableSlot = (UDATA*)((UDATA)clazz + jitVTableOffset);
		VM_AtomicSupport::lockCompareExchange(jitVTableSlot, patchup, thunk);
	}
	currentThread->tempSlot = thunk;
}

#endif /* J9SW_NEEDS_JIT_2_INTERP_THUNKS */

/* New-style helpers */

#if !defined(J9VM_ENV_DATA64)

U_64 J9FASTCALL
fast_jitVolatileReadLong(J9VMThread *currentThread, U_64 *address)
{
	JIT_HELPER_PROLOGUE();
	return longVolatileRead(currentThread, address);
}

void J9FASTCALL
fast_jitVolatileWriteLong(J9VMThread *currentThread, U_64* address, U_64 value)
{
	JIT_HELPER_PROLOGUE();
	longVolatileWrite(currentThread, address, &value);
}

jdouble J9FASTCALL
fast_jitVolatileReadDouble(J9VMThread *currentThread, U_64* address)
{
	JIT_HELPER_PROLOGUE();
	U_64 value = longVolatileRead(currentThread, address);
	return *(jdouble*)&value;
}

void J9FASTCALL
fast_jitVolatileWriteDouble(J9VMThread *currentThread, U_64 *address, jdouble value)
{
	JIT_HELPER_PROLOGUE();
	longVolatileWrite(currentThread, address, (U_64*)&value);
}

#endif /* !J9VM_ENV_DATA64 */

void J9FASTCALL
fast_jitCheckIfFinalizeObject(J9VMThread *currentThread, j9object_t object)
{
	JIT_HELPER_PROLOGUE();
	VM_VMHelpers::checkIfFinalizeObject(currentThread, object);
}

void J9FASTCALL
fast_jitCollapseJNIReferenceFrame(J9VMThread *currentThread)
{
	JIT_HELPER_PROLOGUE();
	/* The direct JNI code has dropped all pushed JNI refs from the stack, and has called this routine on the java stack */
	UDATA *bp = ((UDATA *)(((J9SFJNINativeMethodFrame*)currentThread->sp) + 1)) - 1;
	currentThread->javaVM->internalVMFunctions->returnFromJNI(currentThread, bp);
}

UDATA J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitInstanceOf(J9VMThread *currentThread, j9object_t object, J9Class *castClass)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitInstanceOf(J9VMThread *currentThread, J9Class *castClass, j9object_t object)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
	JIT_HELPER_PROLOGUE();
	UDATA isInstance = 0;
	/* null isn't an instance of anything */
	if (NULL != object) {
		J9Class *instanceClass = J9OBJECT_CLAZZ(currentThread, object);
		if (VM_VMHelpers::inlineCheckCast(instanceClass, castClass)) {
			isInstance = 1;
		}
	}
	return isInstance;
}

UDATA J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitCheckAssignable(J9VMThread *currentThread, J9Class *clazz, J9Class *castClass)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitCheckAssignable(J9VMThread *currentThread, J9Class *castClass, J9Class *clazz)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
	JIT_HELPER_PROLOGUE();
	return VM_VMHelpers::inlineCheckCast(clazz, castClass) ? 1 : 0;
}

UDATA J9FASTCALL
fast_jitObjectHashCode(J9VMThread *currentThread, j9object_t object)
{
	JIT_HELPER_PROLOGUE();
	J9JavaVM *vm = currentThread->javaVM;
	return (UDATA)(IDATA)vm->memoryManagerFunctions->j9gc_objaccess_getObjectHashCode(vm, (J9Object*)object);
}

void* J9FASTCALL
fast_jitNewValue(J9VMThread *currentThread, J9Class *objectClass)
{
//	extern void* slow_jitNewObject(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitNewValueImpl(currentThread, objectClass, true, false))) {
		SET_PARM_COUNT(0);
		slowPath = (void*) old_slow_jitNewValue;
	}
	return slowPath;
}

void* J9FASTCALL
fast_jitNewValueNoZeroInit(J9VMThread *currentThread, J9Class *objectClass)
{
//	extern void* slow_jitNewObjectNoZeroInit(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitNewValueImpl(currentThread, objectClass, true, true))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitNewValueNoZeroInit;
	}
	return slowPath;
}

void* J9FASTCALL
fast_jitNewObject(J9VMThread *currentThread, J9Class *objectClass)
{
//	extern void* slow_jitNewObject(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitNewObjectImpl(currentThread, objectClass, true, false))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitNewObject;
	}
	return slowPath;
}

void* J9FASTCALL
fast_jitNewObjectNoZeroInit(J9VMThread *currentThread, J9Class *objectClass)
{
//	extern void* slow_jitNewObjectNoZeroInit(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitNewObjectImpl(currentThread, objectClass, true, true))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitNewObjectNoZeroInit;
	}
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitANewArray(J9VMThread *currentThread, I_32 size, J9Class *elementClass)
#else /* J9VM_ARCH_X86  || J9VM_ARCH_S390*/
fast_jitANewArray(J9VMThread *currentThread, J9Class *elementClass, I_32 size)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
//	extern void* slow_jitANewArray(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitANewArrayImpl(currentThread, elementClass, size, false))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitANewArray;
	}
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitANewArrayNoZeroInit(J9VMThread *currentThread, I_32 size, J9Class *elementClass)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitANewArrayNoZeroInit(J9VMThread *currentThread, J9Class *elementClass, I_32 size)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
//	extern void* slow_jitANewArrayNoZeroInit(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitANewArrayImpl(currentThread, elementClass, size, true))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitANewArrayNoZeroInit;
	}
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitNewArray(J9VMThread *currentThread, I_32 size, I_32 arrayType)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitNewArray(J9VMThread *currentThread, I_32 arrayType, I_32 size)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
//	extern void* slow_jitNewArray(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitNewArrayImpl(currentThread, arrayType, size, false))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitNewArray;
	}
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitNewArrayNoZeroInit(J9VMThread *currentThread, I_32 size, I_32 arrayType)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitNewArrayNoZeroInit(J9VMThread *currentThread, I_32 arrayType, I_32 size)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
//	extern void* slow_jitNewArrayNoZeroInit(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitNewArrayImpl(currentThread, arrayType, size, true))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitNewArrayNoZeroInit;
	}
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitCheckCast(J9VMThread *currentThread, j9object_t object, J9Class *castClass)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitCheckCast(J9VMThread *currentThread, J9Class *castClass, j9object_t object)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
//	extern void* slow_jitCheckCast(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	/* null can be cast to anything, except if castClass is a primitive VT */
	if (NULL != object) {
		J9Class *instanceClass = J9OBJECT_CLAZZ(currentThread, object);
		if (J9_UNEXPECTED(!VM_VMHelpers::inlineCheckCast(instanceClass, castClass))) {
			SET_PARM_COUNT(0);
			currentThread->floatTemp1 = (void*)castClass;
			currentThread->floatTemp2 = (void*)object;
			slowPath = (void*)old_slow_jitCheckCast;
		}
	}
	/* In the future, Valhalla checkcast must throw an exception on
	 * null-restricted checkedType if object is null.
	 * See issue https://github.com/eclipse-openj9/openj9/issues/19764.
	 */
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitCheckCastForArrayStore(J9VMThread *currentThread, j9object_t object, J9Class *castClass)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitCheckCastForArrayStore(J9VMThread *currentThread, J9Class *castClass, j9object_t object)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
//	extern void* slow_jitCheckCastForArrayStore(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	/* null can be cast to anything, except if castClass is a primitive VT */
	if (NULL != object) {
		J9Class *instanceClass = J9OBJECT_CLAZZ(currentThread, object);
		if (J9_UNEXPECTED(!VM_VMHelpers::inlineCheckCast(instanceClass, castClass))) {
			SET_PARM_COUNT(0);
			slowPath = (void*)old_slow_jitCheckCastForArrayStore;
		}
	}
#if defined(J9VM_OPT_VALHALLA_FLATTENABLE_VALUE_TYPES)
	else if (J9_IS_J9CLASS_PRIMITIVE_VALUETYPE(castClass)) {
		slowPath = (void*)old_slow_jitThrowNullPointerException;
	}
#endif /* defined(J9VM_OPT_VALHALLA_FLATTENABLE_VALUE_TYPES) */
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitLookupInterfaceMethod(J9VMThread *currentThread, void *jitEIP, J9Class *receiverClass, UDATA *indexAndLiteralsEA)
#else /* J9VM_ARCH_X86 */
fast_jitLookupInterfaceMethod(J9VMThread *currentThread, J9Class *receiverClass, UDATA *indexAndLiteralsEA, void *jitEIP)
#endif /* J9VM_ARCH_X86 */
{
//	extern void* slow_jitLookupInterfaceMethod(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	SET_PARM_COUNT(0);
	void *slowPath = (void*)old_slow_jitLookupInterfaceMethod;
	currentThread->floatTemp1 = (void*)receiverClass;
	currentThread->floatTemp2 = (void*)indexAndLiteralsEA;
	currentThread->floatTemp3 = (void*)jitEIP;
	J9Class *interfaceClass = ((J9Class**)indexAndLiteralsEA)[0];
	UDATA iTableOffset = indexAndLiteralsEA[1];
	UDATA vTableOffset = convertITableOffsetToVTableOffset(currentThread, receiverClass, interfaceClass, iTableOffset);
	if (0 != vTableOffset) {
		J9Method* method = *(J9Method**)((UDATA)receiverClass + vTableOffset);
		if (J9_ARE_ANY_BITS_SET(J9_ROM_METHOD_FROM_RAM_METHOD(method)->modifiers, J9AccPublic)) {
			slowPath = NULL;
			JIT_RETURN_UDATA(vTableOffset);
		}
	}
	return slowPath;
}

void* J9FASTCALL
fast_jitMethodMonitorEntry(J9VMThread *currentThread, j9object_t syncObject)
{
//	extern void* slow_jitMethodMonitorEntry(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitMonitorEnterImpl(currentThread, syncObject, true))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitMethodMonitorEntry;
	}
	return slowPath;
}

void* J9FASTCALL
fast_jitMonitorEntry(J9VMThread *currentThread, j9object_t syncObject)
{
//	extern void* slow_jitMonitorEntry(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitMonitorEnterImpl(currentThread, syncObject, false))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitMonitorEntry;
	}
	return slowPath;
}

void* J9FASTCALL
fast_jitMethodMonitorExit(J9VMThread *currentThread, j9object_t syncObject)
{
//	extern void* slow_jitMethodMonitorExit(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitMonitorExitImpl(currentThread, syncObject, true))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitMethodMonitorExit;
	}
	return slowPath;
}

void* J9FASTCALL
fast_jitMonitorExit(J9VMThread *currentThread, j9object_t syncObject)
{
//	extern void* slow_jitMonitorExit(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitMonitorExitImpl(currentThread, syncObject, false))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitMonitorExit;
	}
	return slowPath;
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitTypeCheckArrayStoreWithNullCheck(J9VMThread *currentThread, j9object_t objectBeingStored, j9object_t destinationObject)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitTypeCheckArrayStoreWithNullCheck(J9VMThread *currentThread, j9object_t destinationObject, j9object_t objectBeingStored)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
//	extern void* slow_jitTypeCheckArrayStoreWithNullCheck(J9VMThread *currentThread);
	JIT_HELPER_PROLOGUE();
	void *slowPath = NULL;
	if (J9_UNEXPECTED(fast_jitTypeCheckArrayStoreImpl(currentThread, destinationObject, objectBeingStored))) {
		SET_PARM_COUNT(0);
		slowPath = (void*)old_slow_jitTypeCheckArrayStoreWithNullCheck;
	}
	return slowPath;
}

BOOLEAN J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitAcmpeqHelper(J9VMThread *currentThread, j9object_t lhs, j9object_t rhs)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitAcmpeqHelper(J9VMThread *currentThread, j9object_t rhs, j9object_t lhs)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
	JIT_HELPER_PROLOGUE();

	return currentThread->javaVM->internalVMFunctions->valueTypeCapableAcmp(currentThread, lhs, rhs);
}

BOOLEAN J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitAcmpneHelper(J9VMThread *currentThread, j9object_t lhs, j9object_t rhs)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitAcmpneHelper(J9VMThread *currentThread, j9object_t rhs, j9object_t lhs)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
	JIT_HELPER_PROLOGUE();

	return !currentThread->javaVM->internalVMFunctions->valueTypeCapableAcmp(currentThread, lhs, rhs);
}

void* J9FASTCALL
#if defined(J9VM_ARCH_X86) || defined(J9VM_ARCH_S390)
/* TODO Will be cleaned once all platforms adopt the correct parameter order */
fast_jitTypeCheckArrayStore(J9VMThread *currentThread, j9object_t objectBeingStored, j9object_t destinationObject)
#else /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
fast_jitTypeCheckArrayStore(J9VMThread *currentThread, j9object_t destinationObject, j9object_t objectBeingStored)
#endif /* J9VM_ARCH_X86 || J9VM_ARCH_S390*/
{
	JIT_HELPER_PROLOGUE();
	/* Not omitting NULL check */
	return fast_jitTypeCheckArrayStoreWithNullCheck(currentThread, destinationObject, objectBeingStored);
}

void* J9FASTCALL
old_slow_jitReportFinalFieldModified(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(J9Class*, fieldClass, 1);

	void* oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
	VM_VMHelpers::reportFinalFieldModified(currentThread, fieldClass);
	void* addr = restoreJITResolveFrame(currentThread, oldPC, true, false);

	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitReportInstanceFieldRead(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9JITWatchedInstanceFieldData*, dataBlock, 1);
	DECLARE_JIT_PARM(j9object_t, object, 2);
	J9JavaVM *vm = currentThread->javaVM;
	void *addr = NULL;

	if (J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_GET_FIELD)) {
		if (J9_ARE_ANY_BITS_SET(J9OBJECT_CLAZZ(currentThread, object)->classFlags, J9ClassHasWatchedFields)) {
			void* oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			ALWAYS_TRIGGER_J9HOOK_VM_GET_FIELD(vm->hookInterface, currentThread, dataBlock->method, dataBlock->location, object, dataBlock->offset);
			addr = restoreJITResolveFrame(currentThread, oldPC, true, false);
		}
	}

	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitReportInstanceFieldWrite(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(3);
	DECLARE_JIT_PARM(J9JITWatchedInstanceFieldData*, dataBlock, 1);
	DECLARE_JIT_PARM(j9object_t, object, 2);
	DECLARE_JIT_PARM(void*, valuePointer, 3);
	J9JavaVM *vm = currentThread->javaVM;
	void *addr = NULL;

	if (J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_PUT_FIELD)) {
		if (J9_ARE_ANY_BITS_SET(J9OBJECT_CLAZZ(currentThread, object)->classFlags, J9ClassHasWatchedFields)) {
			void* oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			ALWAYS_TRIGGER_J9HOOK_VM_PUT_FIELD(vm->hookInterface, currentThread, dataBlock->method, dataBlock->location, object, dataBlock->offset, *(U_64*)valuePointer);
			addr = restoreJITResolveFrame(currentThread, oldPC, true, false);
		}
	}

	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitReportStaticFieldRead(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(1);
	DECLARE_JIT_PARM(J9JITWatchedStaticFieldData*, dataBlock, 1);
	J9JavaVM *vm = currentThread->javaVM;
	void *addr = NULL;

	if (J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_GET_STATIC_FIELD)) {
		J9Class *fieldClass = dataBlock->fieldClass;
		if (J9_ARE_ANY_BITS_SET(fieldClass->classFlags, J9ClassHasWatchedFields)) {
			void* oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			/* Ensure that this call blocks before reporting the event if another thread
			 * is initializing the class.
			 */
			if (J9_UNEXPECTED(VM_VMHelpers::classRequiresInitialization(currentThread, fieldClass))) {
				vm->internalVMFunctions->initializeClass(currentThread, fieldClass);
				if (VM_VMHelpers::exceptionPending(currentThread)) {
					goto restore;
				}
				if (VM_VMHelpers::immediateAsyncPending(currentThread)) {
					goto restore;
				}
			}
			ALWAYS_TRIGGER_J9HOOK_VM_GET_STATIC_FIELD(vm->hookInterface, currentThread, dataBlock->method, dataBlock->location, fieldClass, dataBlock->fieldAddress);
restore:
			addr = restoreJITResolveFrame(currentThread, oldPC, true, true);
		}
	}

	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void* J9FASTCALL
old_slow_jitReportStaticFieldWrite(J9VMThread *currentThread)
{
	OLD_SLOW_ONLY_JIT_HELPER_PROLOGUE(2);
	DECLARE_JIT_PARM(J9JITWatchedStaticFieldData*, dataBlock, 1);
	DECLARE_JIT_PARM(void*, valuePointer, 2);
	J9JavaVM *vm = currentThread->javaVM;
	void *addr = NULL;

	if (J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_PUT_STATIC_FIELD)) {
		J9Class *fieldClass = dataBlock->fieldClass;
		if (J9_ARE_ANY_BITS_SET(fieldClass->classFlags, J9ClassHasWatchedFields)) {
			/* Read the data now, as the incoming pointer is potentially into the
			 * java stack, which would be invalidated by the class initialization below.
			 * Ensure the read does not get reordered below by the compiler.
			 */
			U_64 value = *(U_64 volatile *)valuePointer;
			VM_AtomicSupport::compilerReorderingBarrier();
			void* oldPC = buildJITResolveFrameForRuntimeHelper(currentThread, parmCount);
			/* Ensure that this call blocks before reporting the event if another thread
			 * is initializing the class.
			 */
			if (J9_UNEXPECTED(VM_VMHelpers::classRequiresInitialization(currentThread, fieldClass))) {
				vm->internalVMFunctions->initializeClass(currentThread, fieldClass);
				if (VM_VMHelpers::exceptionPending(currentThread)) {
					goto restore;
				}
				if (VM_VMHelpers::immediateAsyncPending(currentThread)) {
					goto restore;
				}
			}
			ALWAYS_TRIGGER_J9HOOK_VM_PUT_STATIC_FIELD(vm->hookInterface, currentThread, dataBlock->method, dataBlock->location, fieldClass, dataBlock->fieldAddress, value);
restore:
			addr = restoreJITResolveFrame(currentThread, oldPC, true, false);
		}
	}

	SLOW_JIT_HELPER_EPILOGUE();
	return addr;
}

void
initPureCFunctionTable(J9JavaVM *vm)
{
	J9JITConfig *jitConfig = vm->jitConfig;

	jitConfig->old_fast_jitNewObject = (void*)old_fast_jitNewObject;
	jitConfig->old_slow_jitNewObject = (void*)old_slow_jitNewObject;
	jitConfig->old_fast_jitNewObjectNoZeroInit = (void*)old_fast_jitNewObjectNoZeroInit;
	jitConfig->old_slow_jitNewObjectNoZeroInit = (void*)old_slow_jitNewObjectNoZeroInit;
	jitConfig->old_fast_jitANewArray = (void*)old_fast_jitANewArray;
	jitConfig->old_slow_jitANewArray = (void*)old_slow_jitANewArray;
	jitConfig->old_fast_jitANewArrayNoZeroInit = (void*)old_fast_jitANewArrayNoZeroInit;
	jitConfig->old_slow_jitANewArrayNoZeroInit = (void*)old_slow_jitANewArrayNoZeroInit;
	jitConfig->old_fast_jitNewArray = (void*)old_fast_jitNewArray;
	jitConfig->old_slow_jitNewArray = (void*)old_slow_jitNewArray;
	jitConfig->old_fast_jitNewArrayNoZeroInit = (void*)old_fast_jitNewArrayNoZeroInit;
	jitConfig->old_slow_jitNewArrayNoZeroInit = (void*)old_slow_jitNewArrayNoZeroInit;
	jitConfig->old_slow_jitAMultiNewArray = (void*)old_slow_jitAMultiNewArray;
	jitConfig->old_slow_jitStackOverflow = (void*)old_slow_jitStackOverflow;
	jitConfig->old_slow_jitResolveString = (void*)old_slow_jitResolveString;
	jitConfig->fast_jitAcquireVMAccess = (void*)fast_jitAcquireVMAccess;
	jitConfig->fast_jitReleaseVMAccess = (void*)fast_jitReleaseVMAccess;
	jitConfig->old_slow_jitCheckAsyncMessages = (void*)old_slow_jitCheckAsyncMessages;
	jitConfig->old_fast_jitCheckCast = (void*)old_fast_jitCheckCast;
	jitConfig->old_slow_jitCheckCast = (void*)old_slow_jitCheckCast;
	jitConfig->old_fast_jitCheckCastForArrayStore = (void*)old_fast_jitCheckCastForArrayStore;
	jitConfig->old_slow_jitCheckCastForArrayStore = (void*)old_slow_jitCheckCastForArrayStore;
	jitConfig->old_fast_jitCheckIfFinalizeObject = (void*)old_fast_jitCheckIfFinalizeObject;
	jitConfig->old_fast_jitCollapseJNIReferenceFrame = (void*)old_fast_jitCollapseJNIReferenceFrame;
	jitConfig->old_slow_jitHandleArrayIndexOutOfBoundsTrap = (void*)old_slow_jitHandleArrayIndexOutOfBoundsTrap;
	jitConfig->old_slow_jitHandleIntegerDivideByZeroTrap = (void*)old_slow_jitHandleIntegerDivideByZeroTrap;
	jitConfig->old_slow_jitHandleNullPointerExceptionTrap = (void*)old_slow_jitHandleNullPointerExceptionTrap;
	jitConfig->old_slow_jitHandleInternalErrorTrap = (void*)old_slow_jitHandleInternalErrorTrap;
	jitConfig->old_fast_jitInstanceOf = (void*)old_fast_jitInstanceOf;
	jitConfig->old_fast_jitLookupInterfaceMethod = (void*)old_fast_jitLookupInterfaceMethod;
	jitConfig->old_slow_jitLookupInterfaceMethod = (void*)old_slow_jitLookupInterfaceMethod;
#if defined(J9VM_OPT_OPENJDK_METHODHANDLE)
	jitConfig->old_fast_jitLookupDynamicPublicInterfaceMethod = (void*)old_fast_jitLookupDynamicPublicInterfaceMethod;
	jitConfig->old_slow_jitLookupDynamicPublicInterfaceMethod = (void*)old_slow_jitLookupDynamicPublicInterfaceMethod;
#endif /* defined(J9VM_OPT_OPENJDK_METHODHANDLE) */
	jitConfig->old_fast_jitMethodIsNative = (void*)old_fast_jitMethodIsNative;
	jitConfig->old_fast_jitMethodIsSync = (void*)old_fast_jitMethodIsSync;
	jitConfig->old_fast_jitMethodMonitorEntry = (void*)old_fast_jitMethodMonitorEntry;
	jitConfig->old_slow_jitMethodMonitorEntry = (void*)old_slow_jitMethodMonitorEntry;
	jitConfig->old_fast_jitMonitorEntry = (void*)old_fast_jitMonitorEntry;
	jitConfig->old_slow_jitMonitorEntry = (void*)old_slow_jitMonitorEntry;
	jitConfig->old_fast_jitMethodMonitorExit = (void*)old_fast_jitMethodMonitorExit;
	jitConfig->old_slow_jitMethodMonitorExit = (void*)old_slow_jitMethodMonitorExit;
	jitConfig->old_slow_jitThrowIncompatibleReceiver = (void*)old_slow_jitThrowIncompatibleReceiver;
	jitConfig->old_fast_jitMonitorExit = (void*)old_fast_jitMonitorExit;
	jitConfig->old_slow_jitMonitorExit = (void*)old_slow_jitMonitorExit;
	jitConfig->old_slow_jitReportMethodEnter = (void*)old_slow_jitReportMethodEnter;
	jitConfig->old_slow_jitReportStaticMethodEnter = (void*)old_slow_jitReportStaticMethodEnter;
	jitConfig->old_slow_jitReportMethodExit = (void*)old_slow_jitReportMethodExit;
	jitConfig->old_slow_jitResolveClass = (void*)old_slow_jitResolveClass;
	jitConfig->old_slow_jitResolveClassFromStaticField = (void*)old_slow_jitResolveClassFromStaticField;
	jitConfig->old_fast_jitResolvedFieldIsVolatile = (void*)old_fast_jitResolvedFieldIsVolatile;
	jitConfig->old_slow_jitResolveField = (void*)old_slow_jitResolveField;
	jitConfig->old_slow_jitResolveFieldSetter = (void*)old_slow_jitResolveFieldSetter;
	jitConfig->old_slow_jitResolveFieldDirect = (void*)old_slow_jitResolveFieldDirect;
	jitConfig->old_slow_jitResolveFieldSetterDirect = (void*)old_slow_jitResolveFieldSetterDirect;
	jitConfig->old_slow_jitResolveStaticField = (void*)old_slow_jitResolveStaticField;
	jitConfig->old_slow_jitResolveStaticFieldSetter = (void*)old_slow_jitResolveStaticFieldSetter;
	jitConfig->old_slow_jitResolveStaticFieldDirect = (void*)old_slow_jitResolveStaticFieldDirect;
	jitConfig->old_slow_jitResolveStaticFieldSetterDirect = (void*)old_slow_jitResolveStaticFieldSetterDirect;
	jitConfig->old_slow_jitResolveInterfaceMethod = (void*)old_slow_jitResolveInterfaceMethod;
	jitConfig->old_slow_jitResolveSpecialMethod = (void*)old_slow_jitResolveSpecialMethod;
	jitConfig->old_slow_jitResolveStaticMethod = (void*)old_slow_jitResolveStaticMethod;
	jitConfig->old_slow_jitResolveVirtualMethod = (void*)old_slow_jitResolveVirtualMethod;
	jitConfig->old_slow_jitResolveMethodType = (void*)old_slow_jitResolveMethodType;
	jitConfig->old_slow_jitResolveMethodHandle = (void*)old_slow_jitResolveMethodHandle;
	jitConfig->old_slow_jitResolveInvokeDynamic = (void*)old_slow_jitResolveInvokeDynamic;
	jitConfig->old_slow_jitResolveConstantDynamic = (void*)old_slow_jitResolveConstantDynamic;
	jitConfig->old_slow_jitResolveHandleMethod = (void*)old_slow_jitResolveHandleMethod;
	jitConfig->old_slow_jitResolveFlattenableField = (void*)old_slow_jitResolveFlattenableField;
	jitConfig->old_slow_jitRetranslateCaller = (void*)old_slow_jitRetranslateCaller;
	jitConfig->old_slow_jitRetranslateCallerWithPreparation = (void*)old_slow_jitRetranslateCallerWithPreparation;
	jitConfig->old_slow_jitRetranslateMethod = (void*)old_slow_jitRetranslateMethod;
	jitConfig->old_slow_jitThrowCurrentException = (void*)old_slow_jitThrowCurrentException;
	jitConfig->old_slow_jitThrowException = (void*)old_slow_jitThrowException;
	jitConfig->old_slow_jitThrowUnreportedException = (void*)old_slow_jitThrowUnreportedException;
	jitConfig->old_slow_jitThrowAbstractMethodError = (void*)old_slow_jitThrowAbstractMethodError;
	jitConfig->old_slow_jitThrowArithmeticException = (void*)old_slow_jitThrowArithmeticException;
	jitConfig->old_slow_jitThrowArrayIndexOutOfBounds = (void*)old_slow_jitThrowArrayIndexOutOfBounds;
	jitConfig->old_slow_jitThrowArrayStoreException = (void*)old_slow_jitThrowArrayStoreException;
	jitConfig->old_slow_jitThrowArrayStoreExceptionWithIP = (void*)old_slow_jitThrowArrayStoreExceptionWithIP;
	jitConfig->old_slow_jitThrowExceptionInInitializerError = (void*)old_slow_jitThrowExceptionInInitializerError;
	jitConfig->old_slow_jitThrowIllegalAccessError = (void*)old_slow_jitThrowIllegalAccessError;
	jitConfig->old_slow_jitThrowIncompatibleClassChangeError = (void*)old_slow_jitThrowIncompatibleClassChangeError;
	jitConfig->old_slow_jitThrowInstantiationException = (void*)old_slow_jitThrowInstantiationException;
	jitConfig->old_slow_jitThrowNullPointerException = (void*)old_slow_jitThrowNullPointerException;
	jitConfig->old_slow_jitThrowWrongMethodTypeException = (void*)old_slow_jitThrowWrongMethodTypeException;
	jitConfig->old_slow_jitThrowIdentityException = (void*)old_slow_jitThrowIdentityException;
	jitConfig->old_fast_jitTypeCheckArrayStoreWithNullCheck = (void*)old_fast_jitTypeCheckArrayStoreWithNullCheck;
	jitConfig->old_slow_jitTypeCheckArrayStoreWithNullCheck = (void*)old_slow_jitTypeCheckArrayStoreWithNullCheck;
	jitConfig->old_fast_jitTypeCheckArrayStore = (void*)old_fast_jitTypeCheckArrayStore;
	jitConfig->old_slow_jitTypeCheckArrayStore = (void*)old_slow_jitTypeCheckArrayStore;
	jitConfig->old_fast_jitWriteBarrierBatchStore = (void*)old_fast_jitWriteBarrierBatchStore;
	jitConfig->old_fast_jitWriteBarrierBatchStoreWithRange = (void*)old_fast_jitWriteBarrierBatchStoreWithRange;
	jitConfig->old_fast_jitWriteBarrierJ9ClassBatchStore = (void*)old_fast_jitWriteBarrierJ9ClassBatchStore;
	jitConfig->old_fast_jitWriteBarrierJ9ClassStore = (void*)old_fast_jitWriteBarrierJ9ClassStore;
	jitConfig->old_fast_jitWriteBarrierStore = (void*)old_fast_jitWriteBarrierStore;
	jitConfig->old_fast_jitWriteBarrierStoreGenerational = (void*)old_fast_jitWriteBarrierStoreGenerational;
	jitConfig->old_fast_jitWriteBarrierStoreGenerationalAndConcurrentMark = (void*)old_fast_jitWriteBarrierStoreGenerationalAndConcurrentMark;
	jitConfig->old_fast_jitWriteBarrierClassStoreMetronome = (void*)old_fast_jitWriteBarrierClassStoreMetronome;
	jitConfig->old_fast_jitWriteBarrierStoreMetronome = (void*)old_fast_jitWriteBarrierStoreMetronome;
	jitConfig->old_slow_jitCallJitAddPicToPatchOnClassUnload = (void*)old_slow_jitCallJitAddPicToPatchOnClassUnload;
	jitConfig->old_slow_jitCallCFunction = (void*)old_slow_jitCallCFunction;
	jitConfig->fast_jitPreJNICallOffloadCheck = (void*)fast_jitPreJNICallOffloadCheck;
	jitConfig->fast_jitPostJNICallOffloadCheck = (void*)fast_jitPostJNICallOffloadCheck;
	jitConfig->old_fast_jitObjectHashCode = (void*)old_fast_jitObjectHashCode;
	jitConfig->old_slow_jitInduceOSRAtCurrentPC = (void*)old_slow_jitInduceOSRAtCurrentPC;
	jitConfig->old_slow_jitInduceOSRAtCurrentPCAndRecompile = (void*)old_slow_jitInduceOSRAtCurrentPCAndRecompile;
	jitConfig->old_slow_jitInterpretNewInstanceMethod = (void*)old_slow_jitInterpretNewInstanceMethod;
	jitConfig->old_slow_jitNewInstanceImplAccessCheck = (void*)old_slow_jitNewInstanceImplAccessCheck;
	jitConfig->old_slow_jitTranslateNewInstanceMethod = (void*)old_slow_jitTranslateNewInstanceMethod;
	jitConfig->old_slow_jitReportFinalFieldModified = (void*)old_slow_jitReportFinalFieldModified;
	jitConfig->old_fast_jitGetFlattenableField = (void*) old_fast_jitGetFlattenableField;
	jitConfig->old_fast_jitCloneValueType = (void*) old_fast_jitCloneValueType;
	jitConfig->old_fast_jitWithFlattenableField = (void*) old_fast_jitWithFlattenableField;
	jitConfig->old_fast_jitPutFlattenableField = (void*) old_fast_jitPutFlattenableField;
	jitConfig->old_fast_jitGetFlattenableStaticField = (void*) old_fast_jitGetFlattenableStaticField;
	jitConfig->old_fast_jitPutFlattenableStaticField = (void*) old_fast_jitPutFlattenableStaticField;
	jitConfig->old_fast_jitLoadFlattenableArrayElement = (void*) old_fast_jitLoadFlattenableArrayElement;
	jitConfig->old_fast_jitStoreFlattenableArrayElement = (void*) old_fast_jitStoreFlattenableArrayElement;
	jitConfig->old_fast_jitAcmpeqHelper = (void*)old_fast_jitAcmpeqHelper;
	jitConfig->old_fast_jitAcmpneHelper = (void*)old_fast_jitAcmpneHelper;
	jitConfig->fast_jitNewValue = (void*)fast_jitNewValue;
	jitConfig->fast_jitNewValueNoZeroInit = (void*)fast_jitNewValueNoZeroInit;
	jitConfig->fast_jitNewObject = (void*)fast_jitNewObject;
	jitConfig->fast_jitNewObjectNoZeroInit = (void*)fast_jitNewObjectNoZeroInit;
	jitConfig->fast_jitANewArray = (void*)fast_jitANewArray;
	jitConfig->fast_jitANewArrayNoZeroInit = (void*)fast_jitANewArrayNoZeroInit;
	jitConfig->fast_jitNewArray = (void*)fast_jitNewArray;
	jitConfig->fast_jitNewArrayNoZeroInit = (void*)fast_jitNewArrayNoZeroInit;
	jitConfig->fast_jitCheckCast = (void*)fast_jitCheckCast;
	jitConfig->fast_jitCheckCastForArrayStore = (void*)fast_jitCheckCastForArrayStore;
	jitConfig->fast_jitMethodMonitorEntry = (void*)fast_jitMethodMonitorEntry;
	jitConfig->fast_jitMonitorEntry = (void*)fast_jitMonitorEntry;
	jitConfig->fast_jitMethodMonitorExit = (void*)fast_jitMethodMonitorExit;
	jitConfig->fast_jitMonitorExit = (void*)fast_jitMonitorExit;
	jitConfig->fast_jitTypeCheckArrayStore = (void*)fast_jitTypeCheckArrayStore;
	jitConfig->fast_jitTypeCheckArrayStoreWithNullCheck = (void*)fast_jitTypeCheckArrayStoreWithNullCheck;
#if !defined(J9VM_ENV_DATA64)
	jitConfig->old_fast_jitVolatileReadLong = (void*)old_fast_jitVolatileReadLong;
	jitConfig->old_fast_jitVolatileWriteLong = (void*)old_fast_jitVolatileWriteLong;
	jitConfig->old_fast_jitVolatileReadDouble = (void*)old_fast_jitVolatileReadDouble;
	jitConfig->old_fast_jitVolatileWriteDouble = (void*)old_fast_jitVolatileWriteDouble;
#endif /* !J9VM_ENV_DATA64 */
#if defined(J9SW_NEEDS_JIT_2_INTERP_THUNKS)
	jitConfig->old_slow_icallVMprJavaSendPatchupVirtual = (void*)old_slow_icallVMprJavaSendPatchupVirtual;
#endif /* J9SW_NEEDS_JIT_2_INTERP_THUNKS */
	jitConfig->c_jitDecompileOnReturn = (void*)c_jitDecompileOnReturn;
	jitConfig->c_jitDecompileAtExceptionCatch = (void*)c_jitDecompileAtExceptionCatch;
	jitConfig->c_jitReportExceptionCatch = (void*)c_jitReportExceptionCatch;
	jitConfig->c_jitDecompileAtCurrentPC = (void*)c_jitDecompileAtCurrentPC;
	jitConfig->c_jitDecompileBeforeReportMethodEnter = (void*)c_jitDecompileBeforeReportMethodEnter;
	jitConfig->c_jitDecompileBeforeMethodMonitorEnter = (void*)c_jitDecompileBeforeMethodMonitorEnter;
	jitConfig->c_jitDecompileAfterAllocation = (void*)c_jitDecompileAfterAllocation;
	jitConfig->c_jitDecompileAfterMonitorEnter = (void*)c_jitDecompileAfterMonitorEnter;
	jitConfig->old_slow_jitReportInstanceFieldRead = (void*)old_slow_jitReportInstanceFieldRead;
	jitConfig->old_slow_jitReportInstanceFieldWrite = (void*)old_slow_jitReportInstanceFieldWrite;
	jitConfig->old_slow_jitReportStaticFieldRead = (void*)old_slow_jitReportStaticFieldRead;
	jitConfig->old_slow_jitReportStaticFieldWrite = (void*)old_slow_jitReportStaticFieldWrite;
}

} /* extern "C" */
