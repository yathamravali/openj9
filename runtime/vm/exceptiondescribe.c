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

#include <string.h>

#include "j9.h"
#include "j9protos.h"
#include "j9consts.h"
#include "j2sever.h"
#include "rommeth.h"
#include "j9cp.h"
#include "j9vmnls.h"
#include "vmaccess.h"
#include "objhelp.h"
#include "vm_internal.h"
#include "jvminit.h"
#include "SCQueryFunctions.h"

typedef UDATA (*callback_func_t) (J9VMThread * vmThread, void * userData, UDATA bytecodeOffset, J9ROMClass * romClass, J9ROMMethod * romMethod, J9UTF8 * fileName, UDATA lineNumber, J9ClassLoader* classLoader, J9Class* ramClass, UDATA frameType);

static void printExceptionInThread (J9VMThread* vmThread);
static UDATA isSubclassOfThreadDeath (J9VMThread *vmThread, j9object_t exception);
static void printExceptionMessage (J9VMThread* vmThread, j9object_t exception);
static J9Class* findJ9ClassForROMClass(J9VMThread *vmThread, J9ROMClass *romClass, J9ClassLoader **resultClassLoader);


/* assumes VM access */
static void
printExceptionInThread(J9VMThread* vmThread)
{
	char* name;
	const char* format;

	PORT_ACCESS_FROM_VMC(vmThread);

	format = j9nls_lookup_message(
		J9NLS_INFO | J9NLS_DO_NOT_PRINT_MESSAGE_TAG | J9NLS_DO_NOT_APPEND_NEWLINE,
		J9NLS_VM_STACK_TRACE_EXCEPTION_IN,
		"Exception in thread \"%s\"");

	name = getOMRVMThreadName(vmThread->omrVMThread);

	j9tty_err_printf(format, name);
	j9tty_err_printf(" ");

	releaseOMRVMThreadName(vmThread->omrVMThread);
}


/* assumes VM access */
static void
printExceptionMessage(J9VMThread* vmThread, j9object_t exception) {
	char stackBuffer[256];
	char* buf = stackBuffer;
	UDATA length = 0;
	const char* separator = "";

	PORT_ACCESS_FROM_VMC(vmThread);

	J9UTF8* exceptionClassName = J9ROMCLASS_CLASSNAME(J9OBJECT_CLAZZ(vmThread, exception)->romClass);
	j9object_t detailMessage = J9VMJAVALANGTHROWABLE_DETAILMESSAGE(vmThread, exception);

	if (NULL != detailMessage) {
		buf = copyStringToUTF8WithMemAlloc(vmThread, detailMessage, J9_STR_NULL_TERMINATE_RESULT, "", 0, stackBuffer, 256, &length);

		if (NULL != buf) {
			separator = ": ";
		}
	}

	j9tty_err_printf("%.*s%s%.*s\n",
		(UDATA)J9UTF8_LENGTH(exceptionClassName),
		J9UTF8_DATA(exceptionClassName),
		separator,
		length,
		buf);

	if (buf != stackBuffer) {
		j9mem_free_memory(buf);
	}
}


/* assumes VM access */
static UDATA
printStackTraceEntry(J9VMThread * vmThread, void * voidUserData, UDATA bytecodeOffset, J9ROMClass *romClass, J9ROMMethod * romMethod, J9UTF8 * sourceFile, UDATA lineNumber, J9ClassLoader* classLoader, J9Class* ramClass, UDATA frameType) {
	const char* format = NULL;
	J9JavaVM *vm = vmThread->javaVM;
	J9InternalVMFunctions * vmFuncs = vm->internalVMFunctions;
	PORT_ACCESS_FROM_JAVAVM(vm);

	if (NULL == romMethod) {
		format = j9nls_lookup_message(
			J9NLS_INFO | J9NLS_DO_NOT_PRINT_MESSAGE_TAG,
			J9NLS_VM_STACK_TRACE_UNKNOWN,
			NULL);
		j9tty_err_printf(format);
	} else {
		J9UTF8* className = J9ROMCLASS_CLASSNAME(romClass);
		J9UTF8* methodName = J9ROMMETHOD_NAME(romMethod);
		char *sourceFileName = NULL;
		UDATA sourceFileNameLen = 0;
		char *moduleNameUTF = NULL;
		char *moduleVersionUTF = NULL;
		char versionBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
		BOOLEAN freeModuleVersion = FALSE;

		if (JAVA_SPEC_VERSION >= 11) {
			if (J9_ARE_ALL_BITS_SET(vm->runtimeFlags, J9_RUNTIME_JAVA_BASE_MODULE_CREATED)) {
				J9Module *module = NULL;
				U_8 *classNameUTF = J9UTF8_DATA(J9ROMCLASS_CLASSNAME(romClass));
				UDATA length = packageNameLength(romClass);

				omrthread_monitor_enter(vm->classLoaderModuleAndLocationMutex);
				module = vmFuncs->findModuleForPackage(vmThread, classLoader, classNameUTF, (U_32) length);
				omrthread_monitor_exit(vm->classLoaderModuleAndLocationMutex);

				if (NULL != module) {
					if (module != vm->javaBaseModule) {
						J9UTF8 *moduleName = module->moduleName;
						if (NULL != moduleName) {
							moduleNameUTF = (char *)J9UTF8_DATA(moduleName);
						}
					} else {
						moduleNameUTF = JAVA_BASE_MODULE;
					}

					if (NULL != moduleNameUTF) {
						moduleVersionUTF = copyStringToUTF8WithMemAlloc(
							vmThread, module->version, J9_STR_NULL_TERMINATE_RESULT, "", 0, versionBuf, J9VM_PACKAGE_NAME_BUFFER_LENGTH, NULL);
						if (NULL == moduleVersionUTF) {
							moduleVersionUTF = JAVA_SPEC_VERSION_STRING;
						} else if (versionBuf != moduleVersionUTF) {
							freeModuleVersion = TRUE;
						}
					}
				}
			} else {
				/* Assume bootstrap code within java.base when the module itself hasn't been created yet  */
				moduleNameUTF = JAVA_BASE_MODULE;
				moduleVersionUTF = JAVA_SPEC_VERSION_STRING;
			}
		}
		if (J9_ARE_ANY_BITS_SET(romMethod->modifiers, J9AccNative)) {
			sourceFileName = "NativeMethod";
			sourceFileNameLen = LITERAL_STRLEN("NativeMethod");
		} else if (NULL != sourceFile) {
			sourceFileName = (char *)J9UTF8_DATA(sourceFile);
			sourceFileNameLen = (UDATA)J9UTF8_LENGTH(sourceFile);
		} else {
			sourceFileName = "Unknown Source";
			sourceFileNameLen = LITERAL_STRLEN("Unknown Source");
		}
		if (NULL != moduleNameUTF) {
			if (0 != lineNumber) {
				format = j9nls_lookup_message(
					J9NLS_DO_NOT_PRINT_MESSAGE_TAG,
					J9NLS_VM_STACK_TRACE_WITH_MODULE_VERSION_LINE,
					"\tat %.*s.%.*s (%s@%s/%.*s:%u)\n");
			} else {
				format = j9nls_lookup_message(
					J9NLS_DO_NOT_PRINT_MESSAGE_TAG,
					J9NLS_VM_STACK_TRACE_WITH_MODULE_VERSION,
					"\tat %.*s.%.*s (%s@%s/%.*s)\n");
			}
			j9tty_err_printf(format,
				(UDATA)J9UTF8_LENGTH(className), J9UTF8_DATA(className),
				(UDATA)J9UTF8_LENGTH(methodName), J9UTF8_DATA(methodName),
				moduleNameUTF, moduleVersionUTF,
				sourceFileNameLen, sourceFileName,
				lineNumber); /* line number will be ignored in if it's not used in the format string */
		} else {
			if (0 != lineNumber) {
				format = j9nls_lookup_message(
					J9NLS_DO_NOT_PRINT_MESSAGE_TAG,
					J9NLS_VM_STACK_TRACE_WITH_LINE,
					"\tat %.*s.%.*s (%.*s:%u)\n");
			} else {
				format = j9nls_lookup_message(
					J9NLS_DO_NOT_PRINT_MESSAGE_TAG,
					J9NLS_VM_STACK_TRACE,
					"\tat %.*s.%.*s (%.*s)\n");
			}
			j9tty_err_printf(format,
				(UDATA)J9UTF8_LENGTH(className), J9UTF8_DATA(className),
				(UDATA)J9UTF8_LENGTH(methodName), J9UTF8_DATA(methodName),
				sourceFileNameLen, sourceFileName,
				lineNumber); /* line number will be ignored in if it's not used in the format string */
		}
		if (freeModuleVersion) {
			j9mem_free_memory(moduleVersionUTF);
		}
	}

	return TRUE;
}

/**
* Find the J9Class given a ROMClass.
* @param[in] vmThread The current VM thread.
* @param[in] ROMClass The J9ROMClass from which the J9Class is created.
* @param[in, out] resultClassLoader This is the classLoader that owns the memory segment that the ROMClass was found in.
*	The romclass memory segment in the shared cache always belongs to vm->systemClassLoader. Set it to the actual class loader which loaded the class in this case.
*
* @return The J9class, or NULL on failure.
*/
static J9Class *
findJ9ClassForROMClass(J9VMThread *vmThread, J9ROMClass *romClass, J9ClassLoader **resultClassLoader)
{
	J9UTF8 const *utfClassName = J9ROMCLASS_CLASSNAME(romClass);
	J9JavaVM *vm = vmThread->javaVM;
	J9SharedClassConfig *config = vm->sharedClassConfig;
	J9Class *ret = NULL;

	if (_J9ROMCLASS_J9MODIFIER_IS_SET(romClass, J9AccClassAnonClass)) {
		/* Anonymous classes are not allowed in any class loader hash table. */
		return NULL;
	}

	if (j9shr_Query_IsAddressInCache(vm, romClass, romClass->romSize)
		&& (NULL != config->romToRamHashTable)
	) {
		J9ClassLoaderWalkState walkState;
		J9ClassLoader *classLoader = NULL;
		BOOLEAN fastMode = J9_ARE_ALL_BITS_SET(vm->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_FAST_CLASS_HASH_TABLE);
		J9Class *ramClass = NULL;
		RomToRamEntry *resultEntry = NULL;
		RomToRamQueryEntry searchEntry;
		searchEntry.romClass = (J9ROMClass *)((UDATA)romClass | ROM_TO_RAM_QUERY_TAG);

		omrthread_rwmutex_enter_read(config->romToRamHashTableMutex);
		resultEntry = hashTableFind(config->romToRamHashTable, &searchEntry);
		omrthread_rwmutex_exit_read(config->romToRamHashTableMutex);
		if (NULL != resultEntry) {
			ret = resultEntry->ramClass;
			*resultClassLoader = ret->classLoader;
			goto done;
		}

		if (!fastMode) {
			omrthread_monitor_enter(vm->classTableMutex);
		}
		/**
		 * Use hashClassTableAt() instead of peekClassHashTable() because classLoaderBlocksMutex must be acquired after classTableMutex.
		 * (allClassLoadersStartDo enters classLoaderBlocksMutex and peekClassHashTable() enters classTableMutex)
		 */
		/**
		 * All ROMClasses from the SCC are owned by the bootstrap class loader. To minimize the chances to iterate all class loaders, probe
		 * the booststrap loader, extensionClassLoader and application loader first to determine if they have the J9Class for the current class.
		 */
		ramClass = hashClassTableAt(*resultClassLoader, (U_8 *)J9UTF8_DATA(utfClassName), J9UTF8_LENGTH(utfClassName));
		if ((NULL != ramClass)
			&& (romClass == ramClass->romClass)
		) {
			ret = ramClass;
			if (!fastMode) {
				omrthread_monitor_exit(vm->classTableMutex);
			}
			goto cacheresult;
		}

		ramClass = hashClassTableAt(vm->extensionClassLoader, (U_8 *)J9UTF8_DATA(utfClassName), J9UTF8_LENGTH(utfClassName));
		if ((NULL != ramClass)
			&& (romClass == ramClass->romClass)
		) {
			ret = ramClass;
			*resultClassLoader = vm->extensionClassLoader;
			if (!fastMode) {
				omrthread_monitor_exit(vm->classTableMutex);
			}
			goto cacheresult;
		}

		ramClass = hashClassTableAt(vm->applicationClassLoader, (U_8 *)J9UTF8_DATA(utfClassName), J9UTF8_LENGTH(utfClassName));
		if ((NULL != ramClass)
			&& (romClass == ramClass->romClass)
		) {
			ret = ramClass;
			*resultClassLoader = vm->applicationClassLoader;
			if (!fastMode) {
				omrthread_monitor_exit(vm->classTableMutex);
			}
			goto cacheresult;
		}

		classLoader = vm->internalVMFunctions->allClassLoadersStartDo(&walkState, vm, 0);
		while (NULL != classLoader) {
			if ((classLoader != *resultClassLoader)
				&& (classLoader != vm->extensionClassLoader)
				&& (classLoader != vm->applicationClassLoader)
			) {
				ramClass = hashClassTableAt(classLoader, (U_8 *)J9UTF8_DATA(utfClassName), J9UTF8_LENGTH(utfClassName));
				if ((NULL != ramClass)
					&& (romClass == ramClass->romClass)
				) {
					ret = ramClass;
					*resultClassLoader = classLoader;
					break;
				}
			}
			classLoader = vm->internalVMFunctions->allClassLoadersNextDo(&walkState);
		}
		vm->internalVMFunctions->allClassLoadersEndDo(&walkState);
		if (!fastMode) {
			omrthread_monitor_exit(vm->classTableMutex);
		}
cacheresult:
		if (NULL != ret) {
			RomToRamEntry newEntry;
			newEntry.ramClass = ret;
			omrthread_rwmutex_enter_write(config->romToRamHashTableMutex);
			hashTableAdd(config->romToRamHashTable, &newEntry);
			omrthread_rwmutex_exit_write(config->romToRamHashTableMutex);
		}
	} else {
		ret = peekClassHashTable(vmThread, *resultClassLoader, (U_8 *)J9UTF8_DATA(utfClassName), J9UTF8_LENGTH(utfClassName));
	}
done:
	return ret;
}

/*
 * Walks the backtrace of an exception instance, invoking a user-supplied callback function for
 * each frame on the call stack.
 *
 * @param vmThread
 * @param exception The exception object that contains the backtrace.
 * @param callback The callback function to be invoked for each stack frame.
 * @param userData Opaque data pointer passed to the callback function.
 * @param pruneConstructors Non-zero if constructors should be pruned from the stack trace.
 * @param sizeOfWalkstateCache Non-zero if exception is walkstate cache instead of exception object.
 * 								Indicatest the size of cache.
 * @return The number of times the callback function was invoked.
 *
 * @note Assumes VM access
 **/
UDATA
iterateStackTraceImpl(J9VMThread * vmThread, j9object_t* exception, callback_func_t callback, void * userData, UDATA pruneConstructors, UDATA skipHiddenFrames, UDATA sizeOfWalkstateCache, BOOLEAN exceptionIsJavaObject)
{
	J9JavaVM * vm = vmThread->javaVM;
	UDATA totalEntries = 0;
	void *walkback = NULL;

	if (exceptionIsJavaObject) {
		walkback = J9VMJAVALANGTHROWABLE_WALKBACK(vmThread, (*exception));
	} else {
		walkback = exception;
	}

	/* Note that exceptionAddr might be a pointer into the current thread's stack, so no java code is allowed to run
	   (nothing which could cause the stack to grow).
	*/

	if (walkback) {
		U_32 arraySize = 0;

		U_32 currentElement = 0;
		UDATA callbackResult = TRUE;
#ifndef J9VM_INTERP_NATIVE_SUPPORT
		pruneConstructors = FALSE;
#endif
		if (exceptionIsJavaObject) {
			arraySize = J9INDEXABLEOBJECT_SIZE(vmThread, walkback);

			/* A zero terminates the stack trace - search backwards through the array to determine the correct size */

			while ((arraySize != 0) && (J9JAVAARRAYOFUDATA_LOAD(vmThread, walkback, arraySize-1)) == 0) {
				--arraySize;
			}
		} else {
			arraySize = (U_32)sizeOfWalkstateCache;
		}

		/* Loop over the stack trace */

		while (currentElement != arraySize) {
			/* write as for or move currentElement++ to very end */
			UDATA methodPC = 0;
			J9ROMMethod * romMethod = NULL;
			J9ROMClass *romClass = NULL;
			UDATA lineNumber = 0;
			J9UTF8 * fileName = NULL;
			J9ClassLoader *classLoader = NULL;
			J9Class *ramClass = NULL;
			UDATA frameType = J9VM_STACK_FRAME_INTERPRETER;
#ifdef J9VM_INTERP_NATIVE_SUPPORT
			J9JITExceptionTable * metaData = NULL;
			UDATA inlineDepth = 0;
			void * inlinedCallSite = NULL;
			void * inlineMap = NULL;
			J9JITConfig * jitConfig = vm->jitConfig;

			if (exceptionIsJavaObject) {
				methodPC = J9JAVAARRAYOFUDATA_LOAD(vmThread, J9VMJAVALANGTHROWABLE_WALKBACK(vmThread, (*exception)), currentElement);
			} else {
				methodPC = ((UDATA *)exception)[currentElement];
			}

			if (jitConfig) {
				metaData = jitConfig->jitGetExceptionTableFromPC(vmThread, methodPC);
				if (metaData) {
					inlineMap = jitConfig->jitGetInlinerMapFromPC(vmThread, vm, metaData, methodPC);
					if (inlineMap) {
						inlinedCallSite = jitConfig->getFirstInlinedCallSite(metaData, inlineMap);
						if (inlinedCallSite) {
							inlineDepth = jitConfig->getJitInlineDepthFromCallSite(metaData, inlinedCallSite);
							totalEntries += inlineDepth;
						}
					}
				}
			}
#endif

			++currentElement; /* can't increment in J9JAVAARRAYOFUDATA_LOAD macro, so must increment here. */
			++totalEntries;
			if ((callback != NULL) || pruneConstructors || skipHiddenFrames) {
#ifdef J9VM_INTERP_NATIVE_SUPPORT
				if (metaData) {
					J9Method *ramMethod;
					UDATA isSameReceiver;
inlinedEntry:
					/* Check for metadata unload */
					if (NULL == metaData->ramMethod) {
						totalEntries = 0;
						goto done;
					}
					if (inlineDepth == 0) {
						if (inlineMap == NULL) {
							methodPC = UDATA_MAX;
							isSameReceiver = FALSE;
						} else {
							methodPC = jitConfig->getCurrentByteCodeIndexAndIsSameReceiver(metaData, inlineMap, NULL, &isSameReceiver);
						}
						ramMethod = metaData->ramMethod;
						frameType = J9VM_STACK_FRAME_JIT;
					} else {
						methodPC = jitConfig->getCurrentByteCodeIndexAndIsSameReceiver(metaData, inlineMap , inlinedCallSite, &isSameReceiver);
						ramMethod = jitConfig->getInlinedMethod(inlinedCallSite);
						frameType = J9VM_STACK_FRAME_JIT_INLINE;
					}
					if (pruneConstructors) {
						if (isSameReceiver) {
							--totalEntries;
							goto nextInline;
						}
						pruneConstructors = FALSE;
					}
					romMethod = getOriginalROMMethodUnchecked(ramMethod);
					ramClass = J9_CLASS_FROM_CP(J9_CP_FROM_METHOD(ramMethod));
					romClass = ramClass->romClass;
					classLoader = ramClass->classLoader;
				} else {
					pruneConstructors = FALSE;
#endif
					romClass = findROMClassFromPC(vmThread, methodPC, &classLoader);
					if (NULL != romClass) {
						ramClass = findJ9ClassForROMClass(vmThread, romClass, &classLoader);
						while (NULL != ramClass) {
							U_32 i = 0;
							J9Method *methods = ramClass->ramMethods;
							UDATA romMethodCount = ramClass->romClass->romMethodCount;
							for (i = 0; i < romMethodCount; ++i) {
								J9ROMMethod *possibleMethod = J9_ROM_METHOD_FROM_RAM_METHOD(&methods[i]);

								/* Note that we cannot use `J9_BYTECODE_START_FROM_ROM_METHOD` here because native method PCs
								 * point to the start of the J9ROMMethod data structure
								 */
								if ((methodPC >= (UDATA)possibleMethod) && (methodPC < (UDATA)J9_BYTECODE_END_FROM_ROM_METHOD(possibleMethod))) {
									romMethod = possibleMethod;
									methodPC -= (UDATA)J9_BYTECODE_START_FROM_ROM_METHOD(romMethod);
									romClass = ramClass->romClass;
									goto foundROMMethod;
								}
							}

							ramClass = ramClass->replacedClass;
						}

						if (NULL == romMethod) {
							romMethod = findROMMethodInROMClass(vmThread, romClass, methodPC);
							if (NULL != romMethod) {
								methodPC -= (UDATA)J9_BYTECODE_START_FROM_ROM_METHOD(romMethod);
							} else {
								methodPC = UDATA_MAX;
							}
						}
foundROMMethod: ;
					} else {
						methodPC = UDATA_MAX;
					}
#ifdef J9VM_INTERP_NATIVE_SUPPORT
				}
#endif
				if (skipHiddenFrames && (NULL != romMethod)) {
					/* Skip Hidden methods and methods from Hidden or Anonymous classes */
					if (J9ROMCLASS_IS_ANON_OR_HIDDEN(romClass) || J9_ARE_ANY_BITS_SET(romMethod->modifiers, J9AccMethodFrameIteratorSkip)) {
						--totalEntries;
						goto nextInline;
					}
				}
#ifdef J9VM_OPT_DEBUG_INFO_SERVER
				if (romMethod != NULL) {
					if (J9_ARE_ALL_BITS_SET(romMethod->modifiers, J9AccNative)) {
						frameType = J9VM_STACK_FRAME_NATIVE;
					}
					lineNumber = getLineNumberForROMClassFromROMMethod(vm, romMethod, romClass, classLoader, methodPC);
					fileName = getSourceFileNameForROMClass(vm, classLoader, romClass);
				}
#endif

				/* Call the callback with the information */

				if (callback != NULL) {
					/* The methodPC is the bytecode offset within the romMethod. */
					callbackResult = callback(vmThread, userData, methodPC, romClass, romMethod, fileName, lineNumber, classLoader, ramClass, frameType);
				}

#ifdef J9VM_OPT_DEBUG_INFO_SERVER
				if (romMethod != NULL) {
					releaseOptInfoBuffer(vm, romClass);
				}
#endif

				/* Abort the walk if the callback said to do so */

				if (!callbackResult) {
					break;
				}

#ifdef J9VM_INTERP_NATIVE_SUPPORT
nextInline:
				if (inlineDepth != 0) {
					/* Check for metadata unload */
					if (NULL == metaData->ramMethod) {
						totalEntries = 0;
						goto done;
					}
					--inlineDepth;
					inlinedCallSite = jitConfig->getNextInlinedCallSite(metaData, inlinedCallSite);
					goto inlinedEntry;
				}
#endif
			}
		}
	}
done:
	return totalEntries;
}

/*
 * Walks the backtrace of an exception instance, invoking a user-supplied callback function for
 * each frame on the call stack.
 *
 * @param vmThread
 * @param exception The exception object that contains the backtrace.
 * @param callback The callback function to be invoked for each stack frame.
 * @param userData Opaque data pointer passed to the callback function.
 * @param pruneConstructors Non-zero if constructors should be pruned from the stack trace.
 * @return The number of times the callback function was invoked.
 *
 * @note Assumes VM access
 **/
UDATA
iterateStackTrace(J9VMThread * vmThread, j9object_t* exception, callback_func_t callback, void * userData, UDATA pruneConstructors, UDATA skipHiddenFrames)
{
	return iterateStackTraceImpl(vmThread, exception, callback, userData, pruneConstructors, skipHiddenFrames, 0, TRUE);
}

/**
 * This is an helper function to call exceptionDescribe indirectly from gpProtectAndRun function.
 *
 * @param entryArg	Current VM Thread (JNIEnv * env)
 */
static UDATA
gpProtectedExceptionDescribe(void *entryArg)
{
	JNIEnv * env = (JNIEnv *)entryArg;

	exceptionDescribe(env);

	return 0;					/* return value required to conform to port library definition */
}

/**
 * Assumes VM access
 *
 * Builds the exception
 *
 * @param env    J9VMThread *
 *
 */
void
internalExceptionDescribe(J9VMThread * vmThread)
{
	/* If the exception is NULL, do nothing.  Do not fetch the exception value into a local here as we do not have VM access yet. */

	if (vmThread->currentException != NULL) {
		J9JavaVM * vm = vmThread->javaVM;
		j9object_t exception;
		J9Class * eiieClass = NULL;

		/* Fetch and clear the current exception */

		exception = (j9object_t) vmThread->currentException;
		vmThread->currentException = NULL;

		/* In non-tiny VMs, do not print exception traces for ThreadDeath or its subclasses. */

		if (isSubclassOfThreadDeath(vmThread, exception)) {
			goto done;
		}

		/*Report surfaced exception */

		TRIGGER_J9HOOK_VM_EXCEPTION_DESCRIBE(vm->hookInterface, vmThread, exception);

		/* Print the exception thread */

		printExceptionInThread(vmThread);

		/* If the VM has completed initialization, try to print it in Java.  If initialization is not complete of the java call fails, print the exception natively. */

		if (vm->runtimeFlags & J9_RUNTIME_INITIALIZED) {
			PUSH_OBJECT_IN_SPECIAL_FRAME(vmThread, exception);
			printStackTrace(vmThread, exception);
			exception = POP_OBJECT_IN_SPECIAL_FRAME(vmThread);
			if (vmThread->currentException == NULL) {
				goto done;
			}
			vmThread->currentException = NULL;
		}

		do {
			/* If -XX:+ShowHiddenFrames option has not been set, skip hidden method frames */
			UDATA skipHiddenFrames = J9_ARE_NO_BITS_SET(vm->runtimeFlags, J9_RUNTIME_SHOW_HIDDEN_FRAMES);

			/* Print the exception class name and detail message */

			printExceptionMessage(vmThread, exception);

			/* Print the stack trace entries */

			iterateStackTrace(vmThread, &exception, printStackTraceEntry, NULL, TRUE, skipHiddenFrames);

			/* If the exception is an instance of ExceptionInInitializerError, print the wrapped exception */

			if (eiieClass == NULL) {
				eiieClass = internalFindKnownClass(vmThread, J9VMCONSTANTPOOL_JAVALANGEXCEPTIONININITIALIZERERROR, J9_FINDKNOWNCLASS_FLAG_EXISTING_ONLY);
				vmThread->currentException = NULL;
			}
			if (J9OBJECT_CLAZZ(vmThread, exception) == eiieClass) {
#if JAVA_SPEC_VERSION >= 12
				exception = J9VMJAVALANGTHROWABLE_CAUSE(vmThread, exception);
#else
				exception = J9VMJAVALANGEXCEPTIONININITIALIZERERROR_EXCEPTION(vmThread, exception);
#endif /* JAVA_SPEC_VERSION */
			} else {
				break;
			}
		} while (exception != NULL);

done: ;
	}
}

/**
 * This function makes sure that call to "internalExceptionDescribe" is gpProtected
 *
 * @param env	Current VM thread (J9VMThread *)
 */
void JNICALL
exceptionDescribe(JNIEnv * env)
{
	J9VMThread * vmThread = (J9VMThread *) env;
	if (vmThread->currentException != NULL) {
		if (vmThread->gpProtected) {
			enterVMFromJNI(vmThread);
			internalExceptionDescribe(vmThread);
			exitVMToJNI(vmThread);
		} else {
			gpProtectAndRun(gpProtectedExceptionDescribe, env, (void *)env);
		}
	}
}


static UDATA
isSubclassOfThreadDeath(J9VMThread *vmThread, j9object_t exception)
{
	J9Class * threadDeathClass = J9VMJAVALANGTHREADDEATH_OR_NULL(vmThread->javaVM);
	J9Class * exceptionClass = J9OBJECT_CLAZZ(vmThread, exception);

	if (threadDeathClass != NULL) {
		UDATA tdDepth;

		if (threadDeathClass == exceptionClass) {
			return TRUE;
		}
		tdDepth = J9CLASS_DEPTH(threadDeathClass);
		if (J9CLASS_DEPTH(exceptionClass) > tdDepth) {
			if (exceptionClass->superclasses[tdDepth] == threadDeathClass) {
				return TRUE;
			}
		}
	}

	return FALSE;
}
