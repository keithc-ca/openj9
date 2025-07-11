/*******************************************************************************
 * Copyright IBM Corp. and others 2017
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

#include "jni.h"
#include "jcl.h"
#include "jclglob.h"
#include "jclprots.h"
#include "jcl_internal.h"
#include "objhelp.h"
#include "rommeth.h"
#include "vmaccess.h"
#include "VMHelpers.hpp"

extern "C" {
/* Internal flag constants, equivalent Java flags defined in StackWalker class. */
#define J9_RETAIN_CLASS_REFERENCE 0x01
#define J9_SHOW_REFLECT_FRAMES    0x02
#define J9_SHOW_HIDDEN_FRAMES     0x04
#if JAVA_SPEC_VERSION >= 21
#define J9_GET_MONITORS           0x08
#endif /* JAVA_SPEC_VERSION >= 21 */
#if JAVA_SPEC_VERSION >= 22
#define J9_DROP_METHOD_INFO       0x10
#endif /* JAVA_SPEC_VERSION >= 22 */
#define J9_GET_CALLER_CLASS       0x20

#define J9_FRAME_VALID            0x80

#define J9_FRAME_COMMON_MASK (J9_RETAIN_CLASS_REFERENCE | J9_SHOW_REFLECT_FRAMES | J9_SHOW_HIDDEN_FRAMES)

#if JAVA_SPEC_VERSION >= 22
#define J9_FRAME_FILTER_MASK (J9_FRAME_COMMON_MASK | J9_GET_MONITORS | J9_DROP_METHOD_INFO)
#elif JAVA_SPEC_VERSION >= 21 /* JAVA_SPEC_VERSION >= 22 */
#define J9_FRAME_FILTER_MASK (J9_FRAME_COMMON_MASK | J9_GET_MONITORS)
#else /* JAVA_SPEC_VERSION >= 21 */
#define J9_FRAME_FILTER_MASK (J9_FRAME_COMMON_MASK)
#endif /* JAVA_SPEC_VERSION >= 22 */

static UDATA stackFrameFilter(J9VMThread *currentThread, J9StackWalkState *walkState);

static UDATA
stackFrameFilter(J9VMThread *currentThread, J9StackWalkState *walkState)
{
	/*
	 * userData1 contains filtering flags, i.e. show hidden, show reflect
	 * userData2 contains stackWalkerMethod, the method name to search for
	 */
	UDATA result = J9_STACKWALK_STOP_ITERATING;

	if (NULL != walkState->userData2) { /* look for stackWalkerMethod */
		J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(walkState->method);
		J9ROMClass *romClass = J9_CLASS_FROM_METHOD(walkState->method)->romClass;

		J9UTF8 *utf = J9ROMMETHOD_NAME(romMethod);
		const char *stackWalkerMethod = (const char *)walkState->userData2;
		result = J9_STACKWALK_KEEP_ITERATING;

		if (0 == compareUTF8Length(J9UTF8_DATA(utf), J9UTF8_LENGTH(utf), (void *)stackWalkerMethod, strlen(stackWalkerMethod))) {
			utf = J9ROMCLASS_CLASSNAME(romClass);
			if (J9UTF8_LITERAL_EQUALS(J9UTF8_DATA(utf), J9UTF8_LENGTH(utf), "java/lang/StackWalker")) {
				walkState->userData2 = NULL; /* Iteration will skip hidden frames and stop at the true caller of stackWalkerMethod. */
			}
		}
	} else if (J9_ARE_NO_BITS_SET((UDATA)walkState->userData1, J9_SHOW_REFLECT_FRAMES | J9_SHOW_HIDDEN_FRAMES)
			&& VM_VMHelpers::isReflectionMethod(currentThread, walkState->method)
	) {
		/* skip reflection/MethodHandleInvoke frames */
		result = J9_STACKWALK_KEEP_ITERATING;
	} else {
		result = J9_STACKWALK_STOP_ITERATING;
	}

	return result;
}

jobject JNICALL
Java_java_lang_StackWalker_walkWrapperImpl(JNIEnv *env, jclass clazz, jint flags, jstring stackWalkerMethod, jobject function)
{
	J9VMThread *vmThread = (J9VMThread *)env;
	J9JavaVM *vm = vmThread->javaVM;

	J9StackWalkState newWalkState;
	J9StackWalkState *walkState = vmThread->stackWalkState;

	Assert_JCL_notNull (stackWalkerMethod);
	/* Consume thread's stateWalkState, push a new entry to thread for use during Java callout. */
	memset(&newWalkState, 0, sizeof(J9StackWalkState));
	newWalkState.previous = walkState;
	vmThread->stackWalkState = &newWalkState;
	walkState->walkThread = vmThread;
	walkState->flags = J9_STACKWALK_ITERATE_FRAMES | J9_STACKWALK_WALK_TRANSLATE_PC
			| J9_STACKWALK_INCLUDE_NATIVES | J9_STACKWALK_VISIBLE_ONLY;
	/* Unless -XX:+ShowHiddenFrames or StackWalker.Option.SHOW_HIDDEN_FRAMES
	 * has been specified, skip hidden method frames.
	 * If this is called from getCallerClass() API, then always skip hidden frames.
	 */
	if (J9_ARE_ANY_BITS_SET((UDATA)flags, J9_GET_CALLER_CLASS)
	|| (J9_ARE_NO_BITS_SET(vm->runtimeFlags, J9_RUNTIME_SHOW_HIDDEN_FRAMES)
		&& J9_ARE_NO_BITS_SET((UDATA)flags, J9_SHOW_HIDDEN_FRAMES))
	) {
		walkState->flags |= J9_STACKWALK_SKIP_HIDDEN_FRAMES;
	}

#if JAVA_SPEC_VERSION >= 21
	J9ObjectMonitorInfo *info = NULL;
	PORT_ACCESS_FROM_JAVAVM(vm);
	if (J9_ARE_ANY_BITS_SET((UDATA)flags, J9_GET_MONITORS)) {
		J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
		IDATA infoLen = vmFuncs->getOwnedObjectMonitors(vmThread, vmThread, NULL, 0, TRUE);
		if (infoLen > 0) {
			info = (J9ObjectMonitorInfo *)j9mem_allocate_memory(infoLen * sizeof(J9ObjectMonitorInfo), J9MEM_CATEGORY_VM_JCL);
			if (NULL != info) {
				IDATA rc = vmFuncs->getOwnedObjectMonitors(vmThread, vmThread, info, infoLen, TRUE);
				if (rc >= 0) {
					walkState->userData3 = info;
					walkState->userData4 = (void *)infoLen;
				}
			} else {
				vmFuncs->setNativeOutOfMemoryError(vmThread, 0, 0);
				return NULL;
			}
		}
	}
#endif /* JAVA_SPEC_VERSION >= 21 */

	walkState->frameWalkFunction = stackFrameFilter;
	const char *walkerMethodChars = env->GetStringUTFChars(stackWalkerMethod, NULL);
	if (NULL == walkerMethodChars) { /* native out of memory exception pending */
		return NULL;
	}
	/* Ensure userData1/2 used by stackFrameFilter function is set properly. */
	walkState->userData1 = (void *)(UDATA)flags;
	walkState->userData2 = (void *)walkerMethodChars;
	UDATA walkStateResult = vm->walkStackFrames(vmThread, walkState);
	Assert_JCL_true(walkStateResult == J9_STACKWALK_RC_NONE);
	walkState->flags |= J9_STACKWALK_RESUME;
	if (J9SF_FRAME_TYPE_END_OF_STACK != walkState->pc) {
		/* indicate the we have the topmost client method's frame */
		walkState->userData1 = (void *)((UDATA)walkState->userData1 | J9_FRAME_VALID);
	}

	jmethodID walkImplMID = JCL_CACHE_GET(env, MID_java_lang_StackWalker_walkImpl);
	if (NULL == walkImplMID) {
		walkImplMID = env->GetStaticMethodID( clazz, "walkImpl", "(Ljava/util/function/Function;J)Ljava/lang/Object;");
		Assert_JCL_notNull(walkImplMID);
		JCL_CACHE_SET(env, MID_java_lang_StackWalker_walkImpl, walkImplMID);
	}
	jobject result = env->CallStaticObjectMethod(clazz, walkImplMID, function, JLONG_FROM_POINTER(walkState));

	if (NULL != walkerMethodChars) {
		env->ReleaseStringUTFChars(stackWalkerMethod, walkerMethodChars);
	}

#if JAVA_SPEC_VERSION >= 21
	if (NULL != info) {
		j9mem_free_memory(info);
	}
#endif /* JAVA_SPEC_VERSION >= 21 */

	vmThread->stackWalkState = newWalkState.previous;

	return result;
}

#if JAVA_SPEC_VERSION >= 19
jobject JNICALL
Java_java_lang_StackWalker_walkContinuationImpl(JNIEnv *env, jclass clazz, jint flags, jobject function, jobject cont)
{
	J9VMThread *vmThread = (J9VMThread *)env;
	J9JavaVM *vm = vmThread->javaVM;

	J9StackWalkState walkState = {0};
	J9VMThread stackThread = {0};
	J9VMEntryLocalStorage els = {0};

	stackThread.javaVM = vm;
	enterVMFromJNI(vmThread);
	j9object_t continuationObject = J9_JNI_UNWRAP_REFERENCE(cont);
	J9VMContinuation *continuation = J9VMJDKINTERNALVMCONTINUATION_VMREF(vmThread, continuationObject);
	vm->internalVMFunctions->copyFieldsFromContinuation(vmThread, &stackThread, &els, continuation);
	exitVMToJNI(vmThread);

	walkState.walkThread = &stackThread;
	walkState.flags = J9_STACKWALK_ITERATE_FRAMES | J9_STACKWALK_WALK_TRANSLATE_PC
			| J9_STACKWALK_INCLUDE_NATIVES | J9_STACKWALK_VISIBLE_ONLY;
	/* Unless -XX:+ShowHiddenFrames or StackWalker.Option.SHOW_HIDDEN_FRAMES
	 * has been specified, skip hidden method frames.
	 */
	if (J9_ARE_NO_BITS_SET(vm->runtimeFlags, J9_RUNTIME_SHOW_HIDDEN_FRAMES)
	&& J9_ARE_NO_BITS_SET((UDATA)flags, J9_SHOW_HIDDEN_FRAMES)
	) {
		walkState.flags |= J9_STACKWALK_SKIP_HIDDEN_FRAMES;
	}
	walkState.frameWalkFunction = stackFrameFilter;

	/* walking unmounted Continuation will not require skipping StackWalker methods */
	walkState.userData1 = (void *)(UDATA)flags;
	walkState.userData2 = NULL;
	UDATA walkStateResult = vm->walkStackFrames(vmThread, &walkState);
	Assert_JCL_true(walkStateResult == J9_STACKWALK_RC_NONE);
	walkState.flags |= J9_STACKWALK_RESUME;

	if (J9SF_FRAME_TYPE_END_OF_STACK != walkState.pc) {
		/* indicate the we have the topmost client method's frame */
		walkState.userData1 = (void *)((UDATA)walkState.userData1 | J9_FRAME_VALID);
	}

	jmethodID walkImplMID = JCL_CACHE_GET(env, MID_java_lang_StackWalker_walkImpl);
	if (NULL == walkImplMID) {
		walkImplMID = env->GetStaticMethodID( clazz, "walkImpl", "(Ljava/util/function/Function;J)Ljava/lang/Object;");
		Assert_JCL_notNull (walkImplMID);
		JCL_CACHE_SET(env, MID_java_lang_StackWalker_walkImpl, walkImplMID);
	}
	jobject result = env->CallStaticObjectMethod(clazz, walkImplMID, function, JLONG_FROM_POINTER(&walkState));

	return result;
}
#endif /* JAVA_SPEC_VERSION >= 19 */

jobject JNICALL
Java_java_lang_StackWalker_getImpl(JNIEnv *env, jobject clazz, jlong walkStateP)
{
	J9VMThread *vmThread = (J9VMThread *)env;
	J9JavaVM *vm = vmThread->javaVM;
	J9InternalVMFunctions const * const vmFuncs = vm->internalVMFunctions;
	J9MemoryManagerFunctions const * const mmFuncs = vm->memoryManagerFunctions;
	J9StackWalkState *walkState = (J9StackWalkState *)(UDATA)walkStateP;
	jobject result = NULL;

	enterVMFromJNI(vmThread);

	if (J9_ARE_NO_BITS_SET((UDATA)walkState->userData1, J9_FRAME_VALID)) {
		/* skip over the current frame */
		walkState->userData1 = (void *)((UDATA)walkState->userData1 & J9_FRAME_FILTER_MASK);
		if (J9_STACKWALK_RC_NONE != vm->walkStackFrames(vmThread, walkState)) {
			vmFuncs->setNativeOutOfMemoryError(vmThread, 0, 0);
			goto _done;
		}
	}
	/* clear the valid bit */
	walkState->userData1 = (void *)((UDATA)walkState->userData1 & J9_FRAME_FILTER_MASK);

	if (J9SF_FRAME_TYPE_END_OF_STACK != walkState->pc) {
		J9Class *frameClass = J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_OR_NULL(vm);
		j9object_t frame = mmFuncs->J9AllocateObject(vmThread, frameClass, J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
		if (NULL == frame) {
			vmFuncs->setHeapOutOfMemoryError(vmThread);
		} else {
			J9ROMMethod *romMethod = getOriginalROMMethod(walkState->method);
			J9Class *ramClass = J9_CLASS_FROM_METHOD(walkState->method);
			J9ROMClass *romClass = ramClass->romClass;
			J9ClassLoader *classLoader = ramClass->classLoader;

			result = vmFuncs->j9jni_createLocalRef(env, frame);
			UDATA bytecodeOffset = walkState->bytecodePCOffset; /* need this for StackFrame */
			UDATA lineNumber = getLineNumberForROMClassFromROMMethod(vm, romMethod, romClass, classLoader, bytecodeOffset);
			PUSH_OBJECT_IN_SPECIAL_FRAME(vmThread, frame);

			/* set the class object if requested */
			if (J9_ARE_ANY_BITS_SET((UDATA)walkState->userData1, J9_RETAIN_CLASS_REFERENCE)) {
				j9object_t classObject = J9VM_J9CLASS_TO_HEAPCLASS(ramClass);
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_DECLARINGCLASS(vmThread, frame, classObject);
			}

#if JAVA_SPEC_VERSION < 22
			bool const includeMethodInfo = true;
#else /* JAVA_SPEC_VERSION < 22 */
			bool const includeMethodInfo = J9_ARE_NO_BITS_SET((UDATA)walkState->userData1, J9_DROP_METHOD_INFO);

			J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_FLAGS(vmThread, frame, (I_32)(IDATA)walkState->userData1);
#endif /* JAVA_SPEC_VERSION < 22 */

			if (includeMethodInfo) {
				/* set bytecode index */
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_BYTECODEINDEX(vmThread, frame, (U_32)bytecodeOffset);

				/* Fill in line number - Java wants -2 for natives, -1 for no line number (which will be 0 coming in from the iterator). */

				if (J9_ARE_ANY_BITS_SET(romMethod->modifiers, J9AccNative)) {
					lineNumber = -2;
				} else if (lineNumber == 0) {
					lineNumber = -1;
				}
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_LINENUMBER(vmThread, frame, (I_32)lineNumber);
			}

			j9object_t stringObject = J9VMJAVALANGCLASSLOADER_CLASSLOADERNAME(vmThread, classLoader->classLoaderObject);
			J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_CLASSLOADERNAME(vmThread, frame, stringObject);

			J9Module *module = ramClass->module;
			if (NULL != module) {
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_FRAMEMODULE(vmThread, frame, module->moduleObject);
			}

			stringObject = VM_VMHelpers::getClassNameString(vmThread, J9VM_J9CLASS_TO_HEAPCLASS(ramClass), JNI_TRUE);
			if (VM_VMHelpers::exceptionPending(vmThread)) {
				goto _pop_frame;
			}
			J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_CLASSNAME(vmThread, PEEK_OBJECT_IN_SPECIAL_FRAME(vmThread, 0), stringObject);

			if (includeMethodInfo) {
				stringObject = mmFuncs->j9gc_createJavaLangStringWithUTFCache(vmThread, J9ROMMETHOD_NAME(romMethod));
				if (VM_VMHelpers::exceptionPending(vmThread)) {
					goto _pop_frame;
				}
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_METHODNAME(vmThread, PEEK_OBJECT_IN_SPECIAL_FRAME(vmThread, 0), stringObject);

				stringObject = mmFuncs->j9gc_createJavaLangStringWithUTFCache(vmThread, J9ROMMETHOD_SIGNATURE(romMethod));
				if (VM_VMHelpers::exceptionPending(vmThread)) {
					goto _pop_frame;
				}
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_METHODSIGNATURE(vmThread, PEEK_OBJECT_IN_SPECIAL_FRAME(vmThread, 0), stringObject);

				stringObject = J9VMJAVALANGCLASS_FILENAMESTRING(vmThread, J9VM_J9CLASS_TO_HEAPCLASS(ramClass));
				if (NULL == stringObject) {
					J9UTF8 *fileName = getSourceFileNameForROMClass(vm, classLoader, romClass);
					if (NULL != fileName) {
						stringObject = mmFuncs->j9gc_createJavaLangString(vmThread, J9UTF8_DATA(fileName), J9UTF8_LENGTH(fileName), J9_STR_TENURE);
						if (VM_VMHelpers::exceptionPending(vmThread)) {
							goto _pop_frame;
						}
						/* Update the cached fileNameString on the class so subsequent calls will find it. */
						J9VMJAVALANGCLASS_SET_FILENAMESTRING(vmThread, J9VM_J9CLASS_TO_HEAPCLASS(ramClass), stringObject);
					}
				}
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_FILENAME(vmThread, PEEK_OBJECT_IN_SPECIAL_FRAME(vmThread, 0), stringObject);
			}

			if (J9ROMMETHOD_IS_CALLER_SENSITIVE(romMethod)) {
				J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_CALLERSENSITIVE(vmThread, PEEK_OBJECT_IN_SPECIAL_FRAME(vmThread, 0), TRUE);
			}

#if JAVA_SPEC_VERSION >= 21
			if (J9_ARE_ANY_BITS_SET((UDATA)walkState->userData1, J9_GET_MONITORS)) {
				J9ObjectMonitorInfo *monitorInfo = (J9ObjectMonitorInfo *)walkState->userData3;
				IDATA *monitorCount = (IDATA *)(&walkState->userData4);

				/* Temp fields to find the number of monitors hold by this frame. */
				J9ObjectMonitorInfo *tempInfo = monitorInfo;
				U_32 count = 0;
				/* Use a while loop as there may be more than one lock taken in a stack frame. */
				while ((0 != *monitorCount) && ((UDATA)tempInfo->depth == walkState->framesWalked)) {
					count += 1;
					tempInfo += 1;
					(*monitorCount) -= 1;
				}
				if (count > 0) {
					J9Class *arrayClass = fetchArrayClass(vmThread, J9VMJAVALANGOBJECT(vm));
					j9object_t monitorArray = mmFuncs->J9AllocateIndexableObject(vmThread, arrayClass, count, J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE);
					if (NULL == monitorArray) {
						vmFuncs->setHeapOutOfMemoryError(vmThread);
						goto _pop_frame;
					}
					J9VMJAVALANGSTACKWALKERSTACKFRAMEIMPL_SET_MONITORS(vmThread, PEEK_OBJECT_IN_SPECIAL_FRAME(vmThread, 0), monitorArray);
					for (U_32 i = 0; i < count; i++) {
						J9JAVAARRAYOFOBJECT_STORE(vmThread, monitorArray, i, monitorInfo->object);
						monitorInfo += 1;
					}

					/* Store the updated progress back in userData for the next callback. */
					walkState->userData3 = monitorInfo;
				}
			}
#endif /* JAVA_SPEC_VERSION >= 21 */

_pop_frame:
			DROP_OBJECT_IN_SPECIAL_FRAME(vmThread);
		}
	}
_done:
	exitVMToJNI(vmThread);

	return result;
}

} /* extern "C" */
