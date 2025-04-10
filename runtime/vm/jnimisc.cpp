/*******************************************************************************
 * Copyright IBM Corp. and others 2012
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

#include "j2sever.h"
#include "j9accessbarrier.h"
#include "j9comp.h"
#include "j9vmnls.h"
#include "ut_j9vm.h"
#include "util_api.h"
#include "vm_api.h"
#include "vm_internal.h"
#include "j9cp.h"
#include "jni.h"
#include "stackwalk.h"
#include "rommeth.h"

#include "AtomicSupport.hpp"
#include "VMHelpers.hpp"
#include "VMAccess.hpp"
#include "ArrayCopyHelpers.hpp"

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
#include "ffi.h"
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

extern "C" {

#if defined(J9VM_OPT_JAVA_OFFLOAD_SUPPORT)
void
javaOffloadSwitchOnWithReason(J9VMThread *currentThread, UDATA reason)
{
	J9JavaVM *vm = currentThread->javaVM;
	if (NULL != vm->javaOffloadSwitchOnWithReasonFunc) {
		if (0 == currentThread->javaOffloadState) {
			vm->javaOffloadSwitchOnWithReasonFunc(currentThread, reason);
		}
		Assert_VM_unequal(currentThread->javaOffloadState & J9_JNI_OFFLOAD_MAX_VALUE, J9_JNI_OFFLOAD_MAX_VALUE);
		currentThread->javaOffloadState += 1;
	}
}

void
javaOffloadSwitchOffWithReason(J9VMThread *currentThread, UDATA reason)
{
	J9JavaVM *vm = currentThread->javaVM;
	if (NULL != vm->javaOffloadSwitchOffWithReasonFunc) {
		currentThread->javaOffloadState -= 1;
		if (0 == currentThread->javaOffloadState) {
			vm->javaOffloadSwitchOffWithReasonFunc(currentThread, reason);
		}
	}
}
#endif /* defined(J9VM_OPT_JAVA_OFFLOAD_SUPPORT) */

/**
 * Get the array class for a J9Class.
 *
 * @param currentThread[in] the current J9VMThread
 * @param elementTypeClass[in] the J9Class for which to fetch the array class
 *
 * @returns the J9Class of the array class, or NULL if an exception occurred during creation
 */
static VMINLINE J9Class*
fetchArrayClass(J9VMThread *currentThread, J9Class *elementTypeClass)
{
	/* Quick check before grabbing the mutex */
	J9Class *resultClass = elementTypeClass->arrayClass;
	if (NULL == resultClass) {
		/* Allocate an array class */
		J9ROMArrayClass* arrayOfObjectsROMClass = (J9ROMArrayClass*)J9ROMIMAGEHEADER_FIRSTCLASS(currentThread->javaVM->arrayROMClasses);
		resultClass = internalCreateArrayClass(currentThread, arrayOfObjectsROMClass, elementTypeClass);
	}
	return resultClass;
}

/**
 * Find the J9ClassLoader to use for the currently-running native method.
 *
 * @param currentThread[in] the current J9VMThread
 *
 * @pre The current thread must have VM access.
 *
 * @returns the J9ClassLoader to use for any class lookups
 */
static J9ClassLoader*
getCurrentClassLoader(J9VMThread *currentThread)
{
	J9JavaVM *vm = currentThread->javaVM;
	J9ClassLoader *classLoader = NULL;
	J9SFJNINativeMethodFrame *nativeMethodFrame = VM_VMHelpers::findNativeMethodFrame(currentThread);
	J9Method *nativeMethod = nativeMethodFrame->method;
	/* JNI 1.2 says: if there is no current native method, use the appClassLoader */
	if (NULL == nativeMethod) {
		/* Some system threads override the spec behaviour */
		if (J9_ARE_ANY_BITS_SET(currentThread->privateFlags, J9_PRIVATE_FLAGS_USE_BOOTSTRAP_LOADER)) {
			classLoader = vm->systemClassLoader;
		} else {
			classLoader = vm->applicationClassLoader;
			/* If the app loader doesn't exist yet, use the boot loader */
			if ((NULL == classLoader)
#if defined(J9VM_OPT_SNAPSHOTS)
				/* Need to preserve the order. Classloaders should not be considered active until
				 * the classloader object is set
				 */
				|| (NULL == classLoader->classLoaderObject)
#endif /* defined(J9VM_OPT_SNAPSHOTS) */
			) {
				classLoader = vm->systemClassLoader;
			}
		}
	} else {
		/* special case - if loadLibraryWithPath is the current native method, use the class loader passed to that native */
		if (J9_ARE_ANY_BITS_SET(nativeMethodFrame->specialFrameFlags, J9_STACK_FLAGS_USE_SPECIFIED_CLASS_LOADER)) {
			/* The current native method has the ClassLoader instance to use as it's second argument */
			j9object_t classLoaderObject = (j9object_t)(currentThread->arg0EA[-1]);
			/* Handle the object reference being redirected by the stack grower */
			if (J9_ARE_ANY_BITS_SET((UDATA)classLoaderObject, 1)) {
				classLoaderObject = *(j9object_t*)((UDATA)classLoaderObject - 1);
			}
			if (NULL == classLoaderObject) {
				/* loader object is null - use the bootClassLoader */
				classLoader = vm->systemClassLoader;
			} else {
				/* loadLibraryWithPath has ensured the object is initialized */
				classLoader = J9VMJAVALANGCLASSLOADER_VMREF(currentThread, classLoaderObject);
			}
		} else {
			/* use the sender (native method) classloader */
			classLoader = J9_CLASS_FROM_METHOD(nativeMethod)->classLoader;
#if JAVA_SPEC_VERSION >= 15
			/* special case - if jdk/internal/loader/NativeLibraries.load(NativeLibraryImpl impl, String name, boolean isBuiltin, boolean isJNI)
			 * is the current native method, use the class loader of "impl.fromClass".
			 * This nativeMethod can't be cached cause HCR might make it invalid.
			 *
			 * Note that in jdk17, the signature of that method changed to
			 *   NativeLibraries.load(NativeLibraryImpl impl, String name, boolean isBuiltin, boolean isJNI, boolean throwOnFailure)
			 */
			if (J9VMJDKINTERNALLOADERNATIVELIBRARIES_LOAD_METHOD(currentThread->javaVM) == nativeMethod) {
				/* The current native method has a NativeLibraryImpl instance as its first argument */
				j9object_t nativeLibraryImplObject = (j9object_t)(currentThread->arg0EA[0]);
				Trc_VM_findNativeLibrariesLoad_nativeMethod(currentThread, nativeMethod, nativeLibraryImplObject, classLoader);
				/* Handle the object reference being redirected by the stack grower */
				if (J9_ARE_ANY_BITS_SET((UDATA)nativeLibraryImplObject, 1)) {
					nativeLibraryImplObject = *(j9object_t*)((UDATA)nativeLibraryImplObject - 1);
				}
				if (NULL != nativeLibraryImplObject) {
					Trc_VM_findNativeLibrariesLoad_nativeLibraryImplObject(currentThread, nativeLibraryImplObject);
					j9object_t fromClassObj = J9VMJDKINTERNALLOADERNATIVELIBRARIESNATIVELIBRARYIMPL_FROMCLASS(currentThread, nativeLibraryImplObject);
					if (NULL != fromClassObj) {
						Trc_VM_findNativeLibrariesLoad_fromClassObj(currentThread, fromClassObj);
						J9Class *fromClass = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, fromClassObj);
						Trc_VM_findNativeLibrariesLoad_fromClass(currentThread, fromClass);
						if ((NULL != fromClass) && (NULL != fromClass->classLoader)) {
							classLoader = fromClass->classLoader;
						}
					}
				}
				Trc_VM_findNativeLibrariesLoad_classLoader(currentThread, classLoader);
			}
#endif /* JAVA_SPEC_VERSION >= 15 */
		}
	}
	return classLoader;
}

/**
 * Find the length of a C string and whether or not there are any characters
 * which required more than a single-byte UTF8 encoding.
 *
 * @param data[in] the character data
 * @param lengthPtr[out] location into which to store the length
 *
 * @returns true if there are any characters in the string >127, false if not
 */
static bool
checkString(const char *data, UDATA *lengthPtr)
{
	UDATA length = 0;
	const char *source = data;
	U_8 check = 0;
	for(;;) {
		char c = *source++;
		if ('\0' == c) {
			break;
		}
		check |= (U_8)c;
		length += 1;
	}
	*lengthPtr = length;
	return J9_ARE_ANY_BITS_SET(check, 0x80);
}

/**
 * Re-encode UTF8 data into its canonical (most compact) representation.
 * If the input string is malformed, print an error message to the console and abort the VM.
 *
 * @param currentThread[in] the current J9VMThread
 * @param data[in] the input data
 * @param length[in] the input length
 * @param compressedLengthPtr[out] location into which to store the length of the canonical representation
 *
 * @returns the canonical UTF string (newly-allocated memory)
 */
U_8*
compressUTF8(J9VMThread *currentThread, U_8 *data, UDATA length, UDATA *compressedLengthPtr)
{
	PORT_ACCESS_FROM_VMC(currentThread);
	U_8 *compressedData = (U_8*)j9mem_allocate_memory(length, OMRMEM_CATEGORY_VM);
	if (NULL == compressedData) {
		gpCheckSetNativeOutOfMemoryError(currentThread, 0, 0);
	} else {
		U_8 *writeCursor = compressedData;
		while (0 != length) {
			U_16 unicode = 0;
			UDATA consumed = VM_VMHelpers::decodeUTF8CharN(data, &unicode, length);
			if (0 == consumed) {
				j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_JNI_INVALID_UTF8);
				Assert_VM_unreachable();
			}
			data += consumed;
			length -= consumed;
			writeCursor += VM_VMHelpers::encodeUTF8Char(unicode, writeCursor);
		}
		*compressedLengthPtr = writeCursor - compressedData;
	}
	return compressedData;
}

/**
 * If necessary, re-encode UTF8 data into its canonical (most compact) representation.
 * If the input string is malformed, print an error message to the console and abort the VM.
 *
 * @param currentThread[in] the current J9VMThread
 * @param cString[in] the input data
 * @param length[in] the input length
 * @param compressedLengthPtr[out] location into which to store the length of the canonical representation
 *
 * @returns the canonical UTF string (either the input data value or newly-allocated memory)
 */
static U_8*
getVerifiedUTF8(J9VMThread *currentThread, const char *cString, UDATA *compressedLengthPtr)
{
	U_8 *data = (U_8*)cString;
	UDATA length = 0;
	/* If there are no characters > 127, the string can be used directly */
	if (checkString(cString, &length)) {
		data = compressUTF8(currentThread, (U_8*)data, length, &length);
	}
	*compressedLengthPtr = length;
	return data;
}

/**
 * Re-encode UTF8 data into its canonical (most compact) representation.
 * If the input string is malformed, encode something valid into the new representation.
 * No error is generated for invalid data.
 *
 * @param source[in] the input data
 * @param sourceLength[in] the input length
 * @param target[out] location into which to store the canonical representation
 *
 * @returns the length of the canonical UTF string
 */
static UDATA
encodeUnverifiedUTF8(const char *source, UDATA sourceLength, U_8 *target)
{
	U_8 *targetStart = target;
	while (0 != sourceLength) {
		U_8 b = *source;
		U_16 unicode = b;
		source += 1;
		sourceLength -= 1;
		if (0 != (b & 0x80)) {
			/* not single byte encoding */
			if (0xE0 == (b & 0xF0)) {
				/* 3 byte encoding */
				if (0 != sourceLength) {
					b = *source;
					if (0x80 == (b & 0xC0)) {
						source += 1;
						sourceLength -= 1;
						unicode = ((unicode & 0x0F) << 6) + (b & 0x3F);
						if (0 != sourceLength) {
							b = *source;
							if (0x80 == (b & 0xC0)) {
								source += 1;
								sourceLength -= 1;
								unicode = (unicode << 6) + (b & 0x3F);
							}
						}
					}
				}
			} else if (0xC0 == (b & 0xE0)) {
				/* 2 byte encoding */
				if (0 != sourceLength) {
					b = *source;
					if (0x80 == (b & 0xC0)) {
						source += 1;
						sourceLength -= 1;
						unicode = ((unicode & 0x1F) << 6) + (b & 0x3F);
					}
				}
			}
		}
		target += VM_VMHelpers::encodeUTF8Char(unicode, target);
	}
	return target - targetStart;
}

jobject JNICALL
allocObject(JNIEnv *env, jclass clazz)
{
	j9object_t resultObject = NULL;
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	j9object_t classObject = J9_JNI_UNWRAP_REFERENCE(clazz);
	J9Class *j9clazz = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, classObject);
	if (VM_VMHelpers::classCanBeInstantiated(j9clazz)) {
		if (VM_VMHelpers::classRequiresInitialization(currentThread, j9clazz)) {
			gpCheckInitialize(currentThread, j9clazz);
			if (VM_VMHelpers::exceptionPending(currentThread)) {
				goto done;
			}
		}
		resultObject = currentThread->javaVM->memoryManagerFunctions->J9AllocateObject(currentThread, j9clazz, J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE);
		if (NULL == resultObject) {
			gpCheckSetHeapOutOfMemoryError(currentThread);
		}
	} else {
		gpCheckSetCurrentException(currentThread, J9_EX_CTOR_CLASS + J9VMCONSTANTPOOL_JAVALANGINSTANTIATIONEXCEPTION, (UDATA*)classObject);
	}
done:
	jobject result = VM_VMHelpers::createLocalRef(env, resultObject);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jsize JNICALL
getArrayLength(JNIEnv *env, jarray array)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	jsize result = (jsize)J9INDEXABLEOBJECT_SIZE(currentThread, J9_JNI_UNWRAP_REFERENCE(array));
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jint JNICALL
jniThrow(JNIEnv *env, jthrowable obj)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	VM_VMHelpers::setExceptionPending(currentThread, J9_JNI_UNWRAP_REFERENCE(obj));
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return 0;
}

jclass JNICALL
getSuperclass(JNIEnv *env, jclass clazz)
{
	j9object_t resultObject = NULL;
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	J9Class *j9clazz = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, J9_JNI_UNWRAP_REFERENCE(clazz));
	if (!J9ROMCLASS_IS_INTERFACE(j9clazz->romClass)) {
		resultObject = J9VM_J9CLASS_TO_HEAPCLASS(VM_VMHelpers::getSuperclass(j9clazz));
	}
	jclass result = (jclass)VM_VMHelpers::createLocalRef(env, resultObject);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jclass JNICALL
getObjectClass(JNIEnv *env, jobject obj)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	j9object_t resultObject = J9VM_J9CLASS_TO_HEAPCLASS(J9OBJECT_CLAZZ(currentThread, J9_JNI_UNWRAP_REFERENCE(obj)));
	jclass result = (jclass)VM_VMHelpers::createLocalRef(env, resultObject);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jint JNICALL
getVersion(JNIEnv *env)
{
#if JAVA_SPEC_VERSION >= 24
	return JNI_VERSION_24;
#elif JAVA_SPEC_VERSION >= 21
	return JNI_VERSION_21;
#elif JAVA_SPEC_VERSION >= 20
	return JNI_VERSION_20;
#elif JAVA_SPEC_VERSION >= 19
	return JNI_VERSION_19;
#elif JAVA_SPEC_VERSION >= 10
	return JNI_VERSION_10;
#else /* JAVA_SPEC_VERSION >= 10 */
	return JNI_VERSION_1_8;
#endif /* JAVA_SPEC_VERSION >= 21 */
}

jsize JNICALL
getStringLength(JNIEnv *env, jstring string)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	jsize result = (jsize)J9VMJAVALANGSTRING_LENGTH(currentThread, J9_JNI_UNWRAP_REFERENCE(string));
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jboolean JNICALL
isAssignableFrom(JNIEnv *env, jclass clazz1, jclass clazz2)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	J9Class *j9clazz1 =  J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, J9_JNI_UNWRAP_REFERENCE(clazz1));
	J9Class *j9clazz2 =  J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, J9_JNI_UNWRAP_REFERENCE(clazz2));
	jboolean result = VM_VMHelpers::inlineCheckCast(j9clazz1, j9clazz2) ? JNI_TRUE : JNI_FALSE;
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jboolean JNICALL
isSameObject(JNIEnv *env, jobject ref1, jobject ref2)
{
	jboolean result = JNI_TRUE;
	if (ref1 != ref2) {
		if (NULL == ref1) {
			/* ref1 != ref2, so ref2 is certainly not NULL */
			if (NULL != *(j9object_t*)ref2) {
				result = JNI_FALSE;
			}
		} else if (NULL == ref2) {
			/* ref1 != ref2, so ref1 is certainly not NULL */
			if (NULL != *(j9object_t*)ref1) {
				result = JNI_FALSE;
			}
		} else {
			J9VMThread *currentThread = (J9VMThread*)env;
			VM_VMAccess::inlineEnterVMFromJNI(currentThread);
			if (*(j9object_t*)ref1 != *(j9object_t*)ref2) {
				result = JNI_FALSE;
			}
			VM_VMAccess::inlineExitVMToJNI(currentThread);
		}
	}
	return result;
}

void JNICALL
deleteLocalRef(JNIEnv *env, jobject localRef)
{
	if (NULL != localRef) {
		J9VMThread *currentThread = (J9VMThread*)env;
		VM_VMAccess::inlineEnterVMFromJNI(currentThread);
		j9jni_deleteLocalRef(env, localRef);
		VM_VMAccess::inlineExitVMToJNI(currentThread);
	}
}

jclass JNICALL
findClass(JNIEnv *env, const char *name)
{
	j9object_t resultObject = NULL;
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	UDATA nameLength = 0;
	U_8 *verifiedName = getVerifiedUTF8(currentThread, name, &nameLength);
	if (NULL != verifiedName) {
		J9ClassLoader *classLoader = getCurrentClassLoader(currentThread);
		U_8 *trimmedName = verifiedName;
		/* Allow class names in the form LName; since JDK does, despite it being expressly against the spec */
		if ((nameLength > 2) && ('L' == trimmedName[0]) && (';' == trimmedName[nameLength - 1])) {
			trimmedName += 1;
			nameLength -= 2;
		}
		J9Class *clazz = internalFindClassUTF8(currentThread, trimmedName, nameLength, classLoader, J9_FINDCLASS_FLAG_THROW_ON_FAIL);
		if (NULL != clazz) {
			if (VM_VMHelpers::classRequiresInitialization(currentThread, clazz)) {
				initializeClass(currentThread, clazz);
				if (VM_VMHelpers::exceptionPending(currentThread)) {
					goto done;
				}
			}
			resultObject = J9VM_J9CLASS_TO_HEAPCLASS(clazz);
		}
done:
		/* If memory was allocated, free it */
		if (verifiedName != (U_8*)name) {
			PORT_ACCESS_FROM_VMC(currentThread);
			j9mem_free_memory(verifiedName);
		}
	}
	jclass result = (jclass)VM_VMHelpers::createLocalRef(env, resultObject);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jstring JNICALL
newStringUTF(JNIEnv *env, const char *bytes)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	j9object_t resultObject = NULL;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	if (NULL != bytes) {
		U_8 *data = (U_8*)bytes;
		UDATA length = 0;
		bool containsHighBytes = checkString(bytes, &length);
		JAVA_OFFLOAD_SWITCH_ON_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_NEW_STRING_UTF, length);
		/* If there are no characters > 127, the string can be used directly */
		if (containsHighBytes) {
			/* Slow case -- verify the encoding, fix if necessary.
			 * String may double in size (invalid singles becoming doubles).
			 */
			data = (U_8*)jniArrayAllocateMemoryFromThread(currentThread, length * 2);
			if (NULL == data) {
				gpCheckSetNativeOutOfMemoryError(currentThread, 0, 0);
				goto done;
			}
			length = encodeUnverifiedUTF8(bytes, length, data);
		}
		resultObject = currentThread->javaVM->memoryManagerFunctions->j9gc_createJavaLangString(currentThread, data, length, J9_STR_INSTRUMENTABLE);
		if (data != (U_8*)bytes) {
			jniArrayFreeMemoryFromThread(currentThread, (void*)data);
		}
done:
		JAVA_OFFLOAD_SWITCH_OFF_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_NEW_STRING_UTF, length);
	}
	jstring result = (jstring)VM_VMHelpers::createLocalRef(env, resultObject);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jobjectArray JNICALL
newObjectArray(JNIEnv *env, jsize length, jclass clazz, jobject initialElement)
{
	j9object_t resultObject = NULL;
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	if (length < 0) {
		gpCheckSetNegativeArraySizeException(currentThread, (I_32)length);
	} else {
		J9Class *j9clazz = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, J9_JNI_UNWRAP_REFERENCE(clazz));
		J9Class *arrayClass = fetchArrayClass(currentThread, j9clazz);
		if (NULL != arrayClass) {
			resultObject = currentThread->javaVM->memoryManagerFunctions->J9AllocateIndexableObject(currentThread, arrayClass, (U_32)length, J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE);
			if (NULL == resultObject) {
				gpCheckSetHeapOutOfMemoryError(currentThread);
			} else if (NULL != initialElement) {
				j9object_t elementObject = J9_JNI_UNWRAP_REFERENCE(initialElement);
				for (jsize i = 0; i < length; ++i) {
					J9JAVAARRAYOFOBJECT_STORE(currentThread, resultObject, i, elementObject);
				}
			}
		}
	}
	jobjectArray result = (jobjectArray)VM_VMHelpers::createLocalRef(env, resultObject);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

static VMINLINE void
getOrSetArrayRegion(JNIEnv *env, jarray array, jsize start, jsize len, void *buf, bool isGet)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	j9object_t arrayObject = J9_JNI_UNWRAP_REFERENCE(array);
	UDATA size = J9INDEXABLEOBJECT_SIZE(currentThread, arrayObject);
	UDATA ustart = (UDATA)(IDATA)start;
	UDATA ulen = (UDATA)(IDATA)len;
	UDATA end = ustart + ulen;
	if ((ustart >= size)
		|| (end > size)
		|| (end < ustart) /* overflow */
	) {
		if ((ustart != size) || (0 != ulen)) {
			gpCheckSetCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, NULL);
		}
	} else {
		UDATA logElementSize = ((J9ROMArrayClass*)J9OBJECT_CLAZZ(currentThread, arrayObject)->romClass)->arrayShape & 0x0000FFFF;
		UDATA byteCount = ulen << logElementSize;
		/* No guarantee of native memory alignment, so copy byte-wise */
		if (isGet) {
			JAVA_OFFLOAD_SWITCH_ON_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_ARRAY_REGION, byteCount);
			VM_ArrayCopyHelpers::memcpyFromArray(currentThread, arrayObject, (UDATA)0, ustart << logElementSize, byteCount, buf);
			JAVA_OFFLOAD_SWITCH_OFF_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_ARRAY_REGION, byteCount);
		} else {
			JAVA_OFFLOAD_SWITCH_ON_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_SET_ARRAY_REGION, byteCount);
			VM_ArrayCopyHelpers::memcpyToArray(currentThread, arrayObject, (UDATA)0, ustart << logElementSize, byteCount, buf);
			JAVA_OFFLOAD_SWITCH_OFF_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_SET_ARRAY_REGION, byteCount);
		}
	}
	VM_VMAccess::inlineExitVMToJNI(currentThread);
}

void JNICALL
getArrayRegion(JNIEnv *env, jarray array, jsize start, jsize len, void *buf)
{
	getOrSetArrayRegion(env, array, start, len, buf, true);
}

void JNICALL
setArrayRegion(JNIEnv *env, jarray array, jsize start, jsize len, void *buf)
{
	getOrSetArrayRegion(env, array, start, len, buf, false);
}

static void*
getArrayElementsImpl(JNIEnv *env, jarray array, jboolean *isCopy, jboolean ensureMem32)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	J9JavaVM *vm = currentThread->javaVM;
	void *elems = NULL;
	if (!ensureMem32 && J9_ARE_ANY_BITS_SET(vm->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_ALWAYS_USE_JNI_CRITICAL)) {
		elems = vm->memoryManagerFunctions->j9gc_objaccess_jniGetPrimitiveArrayCritical(currentThread, array, isCopy);
	} else {
		VM_VMAccess::inlineEnterVMFromJNI(currentThread);
		elems = memcpyFromHeapArray(currentThread, J9_JNI_UNWRAP_REFERENCE(array), ensureMem32);
		if ((NULL != elems) && (NULL != isCopy)) {
			*isCopy = JNI_TRUE;
		}
		VM_VMAccess::inlineExitVMToJNI(currentThread);
	}
	return elems;
}

void* JNICALL
getArrayElements(JNIEnv *env, jarray array, jboolean *isCopy)
{
	return getArrayElementsImpl(env, array, isCopy, JNI_FALSE);
}

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
void* JNICALL
getArrayElements31(JNIEnv *env, jarray array, jboolean *isCopy)
{
	return getArrayElementsImpl(env, array, isCopy, JNI_TRUE);
}
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

static void
releaseArrayElementsImpl(JNIEnv *env, jarray array, void *elems, jint mode, jboolean ensureMem32)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	J9JavaVM *vm = currentThread->javaVM;
	if (!ensureMem32 && J9_ARE_ANY_BITS_SET(vm->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_ALWAYS_USE_JNI_CRITICAL)) {
		vm->memoryManagerFunctions->j9gc_objaccess_jniReleasePrimitiveArrayCritical(currentThread, array, elems, mode);
	} else {
		VM_VMAccess::inlineEnterVMFromJNI(currentThread);
		memcpyToHeapArray(currentThread, J9_JNI_UNWRAP_REFERENCE(array), elems, mode, ensureMem32);
		VM_VMAccess::inlineExitVMToJNI(currentThread);
	}
}

void JNICALL
releaseArrayElements(JNIEnv *env, jarray array, void *elems, jint mode)
{
	releaseArrayElementsImpl(env, array, elems, mode, JNI_FALSE);
}

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
void JNICALL
releaseArrayElements31(JNIEnv *env, jarray array, void *elems, jint mode)
{
	releaseArrayElementsImpl(env, array, elems, mode, JNI_TRUE);
}
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

jobject
newBaseTypeArray(JNIEnv *env, IDATA length, UDATA arrayClassOffset)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	j9object_t resultObject = NULL;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	if (length < 0) {
		gpCheckSetNegativeArraySizeException(currentThread, (I_32)length);
	} else {
		J9JavaVM *vm = currentThread->javaVM;
		J9Class *arrayClass = *(J9Class**)((UDATA)vm + arrayClassOffset);
		resultObject = vm->memoryManagerFunctions->J9AllocateIndexableObject(currentThread, arrayClass, (U_32)length, J9_GC_ALLOCATE_OBJECT_INSTRUMENTABLE);
		if (NULL == resultObject) {
			gpCheckSetHeapOutOfMemoryError(currentThread);
		}
	}
	jobject result = VM_VMHelpers::createLocalRef(env, resultObject);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

static const jchar*
getStringCharsImpl(JNIEnv *env, jstring string, jboolean *isCopy, jboolean ensureMem32)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	J9JavaVM *vm = currentThread->javaVM;
	const jchar *chars = NULL;
	if (!ensureMem32 && J9_ARE_ANY_BITS_SET(vm->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_ALWAYS_USE_JNI_CRITICAL)) {
		chars = vm->memoryManagerFunctions->j9gc_objaccess_jniGetStringCritical(currentThread, string, isCopy);
	} else {
		VM_VMAccess::inlineEnterVMFromJNI(currentThread);
		j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(string);
		UDATA length = (UDATA)J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject);
		UDATA allocateLength = (length + 1) * 2;

		if (ensureMem32) {
			chars = (jchar*)jniArrayAllocateMemory32FromThread(currentThread, allocateLength);
		} else {
			chars = (jchar*)jniArrayAllocateMemoryFromThread(currentThread, allocateLength);
		}

		if (NULL == chars) {
			gpCheckSetNativeOutOfMemoryError(currentThread, 0, 0);
		} else {
			JAVA_OFFLOAD_SWITCH_ON_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_CHARS, allocateLength);
			/* We just allocated the memory, it will certainly be 2-aligned */
			j9object_t charArray = J9VMJAVALANGSTRING_VALUE(currentThread, stringObject);

			if (IS_STRING_COMPRESSED(currentThread, stringObject)) {
				for (UDATA i = 0; i < length; ++i) {
					((jchar*)chars)[i] = (jchar)(U_8)J9JAVAARRAYOFBYTE_LOAD(currentThread, charArray, i);
				}
			} else {
				VM_ArrayCopyHelpers::memcpyFromArray(currentThread, charArray, (UDATA)1, 0, length, (void*)chars);
			}

			((jchar*)chars)[length] = (jchar)0;
			if (NULL != isCopy) {
				*isCopy = JNI_TRUE;
			}
			JAVA_OFFLOAD_SWITCH_OFF_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_CHARS, allocateLength);
		}
		VM_VMAccess::inlineExitVMToJNI(currentThread);
	}
	return chars;
}

const jchar* JNICALL
getStringChars(JNIEnv *env, jstring string, jboolean *isCopy)
{
	return getStringCharsImpl(env, string, isCopy, JNI_FALSE);
}

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
const jchar* JNICALL
getStringChars31(JNIEnv *env, jstring string, jboolean *isCopy)
{
	return getStringCharsImpl(env, string, isCopy, JNI_TRUE);
}
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

static void
releaseStringCharsImpl(JNIEnv *env, jstring string, const jchar *chars, jboolean ensureMem32)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	J9JavaVM *vm = currentThread->javaVM;
	if (!ensureMem32 && J9_ARE_ANY_BITS_SET(vm->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_ALWAYS_USE_JNI_CRITICAL)) {
		vm->memoryManagerFunctions->j9gc_objaccess_jniReleaseStringCritical(currentThread, string, chars);
	} else {
		/* Allow the VM to crash on NULL input if -Xfuture is enabled */
		if (J9_ARE_ANY_BITS_SET(vm->runtimeFlags, J9_RUNTIME_XFUTURE) || (NULL != chars)) {
			if (ensureMem32) {
				jniArrayFreeMemory32FromThread(currentThread, (void*)chars);
			} else {
				jniArrayFreeMemoryFromThread(currentThread, (void*)chars);
			}
		}
	}
}

void JNICALL
releaseStringChars(JNIEnv *env, jstring string, const jchar *chars)
{
	releaseStringCharsImpl(env, string, chars, JNI_FALSE);
}

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
void JNICALL
releaseStringChars31(JNIEnv *env, jstring string, const jchar *chars)
{
	releaseStringCharsImpl(env, string, chars, JNI_TRUE);
}
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

static void
releaseStringCharsUTFImpl(JNIEnv *env, jstring string, const char *chars, jboolean ensureMem32)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	J9JavaVM *vm = currentThread->javaVM;
	/* Allow the VM to crash on NULL input if -Xfuture is enabled */
	if (J9_ARE_ANY_BITS_SET(vm->runtimeFlags, J9_RUNTIME_XFUTURE) || (NULL != chars)) {
		if (ensureMem32) {
			jniArrayFreeMemory32FromThread(currentThread, (void*)chars);
		} else {
			jniArrayFreeMemoryFromThread(currentThread, (void*)chars);
		}
	}
}

void JNICALL
releaseStringCharsUTF(JNIEnv *env, jstring string, const char *chars)
{
	releaseStringCharsUTFImpl(env, string, chars, JNI_FALSE);
}

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
void JNICALL
releaseStringCharsUTF31(JNIEnv *env, jstring string, const char *chars)
{
	releaseStringCharsUTFImpl(env, string, chars, JNI_TRUE);
}
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

jsize JNICALL
getStringUTFLength(JNIEnv *env, jstring string)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(string);

	U_64 utfLength = getStringUTF8LengthTruncated(currentThread, stringObject, INT32_MAX);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return (jsize)utfLength;
}

#if JAVA_SPEC_VERSION >= 24
jlong JNICALL
getStringUTFLengthAsLong(JNIEnv *env, jstring string)
{
	J9VMThread *currentThread = (J9VMThread *)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(string);

	U_64 utfLength = getStringUTF8LengthTruncated(currentThread, stringObject, I_64_MAX);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return (jlong)utfLength;
}
#endif /* JAVA_SPEC_VERSION >= 24 */

static const char*
getStringUTFCharsImpl(JNIEnv *env, jstring string, jboolean *isCopy, jboolean ensureMem32)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(string);

	UDATA utfLength = getStringUTF8Length(currentThread, stringObject);
	U_8 *utfChars = NULL;
	if (utfLength < UDATA_MAX) {
		/* Add 1 for a null terminator. */
		utfLength += 1;
		if (ensureMem32) {
			utfChars = (U_8 *)jniArrayAllocateMemory32FromThread(currentThread, utfLength);
		} else {
			utfChars = (U_8 *)jniArrayAllocateMemoryFromThread(currentThread, utfLength);
		}
	}

	if (NULL == utfChars) {
		gpCheckSetNativeOutOfMemoryError(currentThread, 0, 0);
	} else {
		JAVA_OFFLOAD_SWITCH_ON_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_UTF_CHARS, utfLength);
		copyStringToUTF8Helper(currentThread, stringObject, J9_STR_NULL_TERMINATE_RESULT, 0, J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject), utfChars, utfLength);

		if (NULL != isCopy) {
			*isCopy = JNI_TRUE;
		}
		JAVA_OFFLOAD_SWITCH_OFF_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_UTF_CHARS, utfLength);
	}
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return (const char*)utfChars;
}

const char* JNICALL
getStringUTFChars(JNIEnv *env, jstring string, jboolean *isCopy)
{
	return getStringUTFCharsImpl(env, string, isCopy, JNI_FALSE);
}

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
const char* JNICALL
getStringUTFChars31(JNIEnv *env, jstring string, jboolean *isCopy)
{
	return getStringUTFCharsImpl(env, string, isCopy, JNI_TRUE);
}
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

void JNICALL
getStringUTFRegion(JNIEnv *env, jstring str, jsize start, jsize len, char *buf)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	if ((start < 0)
		|| (len < 0)
		|| (((U_32) (start + len)) > I_32_MAX)
	) {
outOfBounds:
		gpCheckSetCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGSTRINGINDEXOUTOFBOUNDSEXCEPTION, NULL);
	} else {
		j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(str);
		I_32 length = J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject);
		if ((start + len) > length) {
			goto outOfBounds;
		}
		JAVA_OFFLOAD_SWITCH_ON_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_UTF_REGION, (UDATA)length * sizeof(U_16));
		copyStringToUTF8Helper(currentThread, stringObject, J9_STR_NULL_TERMINATE_RESULT, start, len, (U_8*)buf, UDATA_MAX);
		JAVA_OFFLOAD_SWITCH_OFF_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_UTF_REGION, (UDATA)length * sizeof(U_16));
	}
	VM_VMAccess::inlineExitVMToJNI(currentThread);
}

jint JNICALL
registerNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint nMethods)
{
	jint result = JNI_ERR;
	J9VMThread *currentThread = (J9VMThread*)env;
	J9JavaVM *vm = currentThread->javaVM;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	J9Class *j9clazz = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, J9_JNI_UNWRAP_REFERENCE(clazz));
	/* Make a copy of the incoming method array as this code may modify it */
	PORT_ACCESS_FROM_VMC(currentThread);
	UDATA allocateLength = sizeof(JNINativeMethod) * nMethods;
	JNINativeMethod *copiedNativeMethods = (JNINativeMethod*)j9mem_allocate_memory(allocateLength, OMRMEM_CATEGORY_VM);
	if (NULL == copiedNativeMethods) {
		gpCheckSetNativeOutOfMemoryError(currentThread, 0, 0);
	} else {
		memcpy(copiedNativeMethods, methods, allocateLength);
		/* First verify that each method exists and is native, then apply JVMTI translations and JNI redirection if necessary */
		jint count = nMethods;
		JNINativeMethod *nativeMethod = copiedNativeMethods;
		while (0 != count) {
			J9Method *method = findJNIMethod(currentThread, j9clazz, nativeMethod->name, nativeMethod->signature);
			if ((NULL == method) || (J9_ARE_NO_BITS_SET(J9_ROM_METHOD_FROM_RAM_METHOD(method)->modifiers, J9AccNative))) {
				gpCheckSetCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNOSUCHMETHODERROR, NULL);
				goto done;
			} else {
				void *jniAddress = nativeMethod->fnPtr;

#if defined(J9VM_OPT_JAVA_OFFLOAD_SUPPORT)
				/* Force the native registered via registerNatives() to run on GP */
				if (J9_ARE_ALL_BITS_SET(vm->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_RESTRICT_IFA)) {
					atomicOrIntoConstantPool(vm, method, J9_STARTPC_NATIVE_REQUIRES_SWITCHING);
				}
#endif /* J9VM_OPT_JAVA_OFFLOAD_SUPPORT */

				TRIGGER_J9HOOK_VM_JNI_NATIVE_BIND(vm->hookInterface, currentThread, method, jniAddress);
#if defined(J9VM_NEEDS_JNI_REDIRECTION)
				if (J9_ARE_ANY_BITS_SET((UDATA)jniAddress, J9JNIREDIRECT_REQUIRED_ALIGNMENT - 1)) {
					jniAddress = alignJNIAddress(vm, jniAddress, J9_CLASS_FROM_METHOD(method)->classLoader);
					if (NULL == jniAddress) {
						gpCheckSetNativeOutOfMemoryError(currentThread, 0, 0);
						goto done;
					}
				}
#endif /* J9VM_NEEDS_JNI_REDIRECTION */
				nativeMethod->fnPtr = jniAddress;
			}
			count -= 1;
			nativeMethod += 1;
		}
		/* Now hammer the native addresses in */
		acquireExclusiveVMAccess(currentThread);
		count = nMethods;
		nativeMethod = copiedNativeMethods;
		while (0 != count) {
			void *fnPtr = J9_COMPATIBLE_FUNCTION_POINTER(nativeMethod->fnPtr);
			J9Method *method = findJNIMethod(currentThread, j9clazz, nativeMethod->name, nativeMethod->signature);
			TRIGGER_J9HOOK_VM_JNI_NATIVE_REGISTERED(vm->hookInterface, currentThread, method, fnPtr);
			/* Don't modify methods that have been JITted */
			if (!VM_VMHelpers::methodHasBeenCompiled(method)) {
				/* Mark the method as a JNI native */
				atomicOrIntoConstantPool(vm, method, J9_STARTPC_JNI_NATIVE);
				method->extra = (void*)((UDATA)fnPtr | J9_STARTPC_NOT_TRANSLATED);
				method->methodRunAddress = vm->jniSendTarget;
			}
			count -= 1;
			nativeMethod += 1;
		}
		releaseExclusiveVMAccess(currentThread);
		result = JNI_OK;
done:
		j9mem_free_memory(copiedNativeMethods);
	}
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return result;
}

jint JNICALL
unregisterNatives(JNIEnv *env, jclass clazz)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	J9JavaVM *vm = currentThread->javaVM;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	J9Class *j9clazz = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, J9_JNI_UNWRAP_REFERENCE(clazz));
	acquireExclusiveVMAccess(currentThread);
	J9Method *currentMethod = j9clazz->ramMethods;
	J9Method *endOfMethods = currentMethod + j9clazz->romClass->romMethodCount;

	if (
			(NULL != vm->jitConfig)  &&
			(NULL != vm->jitConfig->jitDiscardPendingCompilationsOfNatives)
	) {
		vm->jitConfig->jitDiscardPendingCompilationsOfNatives(currentThread, j9clazz);
	}
	while (currentMethod != endOfMethods) {
		J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(currentMethod);
		if (J9_ARE_ANY_BITS_SET(romMethod->modifiers, J9AccNative)) {
			VM_AtomicSupport::bitAnd((UDATA*)&currentMethod->constantPool, ~(UDATA)J9_STARTPC_JNI_NATIVE);
			currentMethod->extra = (void*)J9_STARTPC_NOT_TRANSLATED;
			initializeMethodRunAddressNoHook(vm, currentMethod);
		}
		currentMethod += 1;
	}
	releaseExclusiveVMAccess(currentThread);
	VM_VMAccess::inlineExitVMToJNI(currentThread);
	return JNI_OK;
}

void JNICALL
getStringRegion(JNIEnv *env, jstring str, jsize start, jsize len, jchar *buf)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	VM_VMAccess::inlineEnterVMFromJNI(currentThread);
	if ((start < 0)
		|| (len < 0)
		|| (((U_32) (start + len)) > I_32_MAX)
	) {
outOfBounds:
		gpCheckSetCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGSTRINGINDEXOUTOFBOUNDSEXCEPTION, NULL);
	} else {
		j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(str);
		j9object_t charArray = J9VMJAVALANGSTRING_VALUE(currentThread, stringObject);

		UDATA length = (UDATA)J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject);
		UDATA ustart = (UDATA)(IDATA)start;
		UDATA ulen = (UDATA)(IDATA)len;
		if ((ustart + ulen) > length) {
			goto outOfBounds;
		}
		UDATA byteCount = ulen * sizeof(U_16);
		JAVA_OFFLOAD_SWITCH_ON_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_REGION, byteCount);
		if (IS_STRING_COMPRESSED(currentThread, stringObject)) {
			for (jsize i = 0; i < len; ++i) {
				buf[i] = (jchar)(U_8)J9JAVAARRAYOFBYTE_LOAD(currentThread, charArray, i + start);
			}
		} else {
			/* No guarantee of native memory alignment, so copy byte-wise */
			VM_ArrayCopyHelpers::memcpyFromArray(currentThread, charArray, (UDATA)0, start * sizeof(U_16), byteCount, (void*)buf);
		}
		JAVA_OFFLOAD_SWITCH_OFF_WITH_REASON_IF_LIMIT_EXCEEDED(currentThread, J9_JNI_OFFLOAD_SWITCH_GET_STRING_REGION, byteCount);
	}
	VM_VMAccess::inlineExitVMToJNI(currentThread);
}

/* NOTE: This routine does not pop any physically allocated frames - it is
 * used by the finalizer to reset its jni reference allocation space.
 */
void
jniResetStackReferences(JNIEnv *env)
{
	J9VMThread *currentThread = (J9VMThread*)env;
	Assert_VM_mustHaveVMAccess(currentThread);
	J9SFJNINativeMethodFrame *nativeMethodFrame = VM_VMHelpers::findNativeMethodFrame(currentThread);
	UDATA flags = nativeMethodFrame->specialFrameFlags;
	if (J9_ARE_ANY_BITS_SET(flags, J9_SSF_CALL_OUT_FRAME_ALLOC)) {
		jniPopFrame(currentThread, JNIFRAME_TYPE_INTERNAL);
	}
	UDATA bits = J9_SSF_CALL_OUT_FRAME_ALLOC | J9_SSF_JNI_PUSHED_REF_COUNT_MASK;
	if (NULL == nativeMethodFrame->method) {
		if (J9_ARE_ANY_BITS_SET(flags, J9_SSF_JNI_REFS_REDIRECTED)) {
			bits |=  J9_SSF_JNI_REFS_REDIRECTED;
			UDATA *bp = ((UDATA*)(nativeMethodFrame + 1)) - 1;
			freeStacks(currentThread, bp);
		}
	}
	nativeMethodFrame->specialFrameFlags = flags & ~bits;
	currentThread->sp = (UDATA*)((UDATA)currentThread->sp + (UDATA)currentThread->literals);
	currentThread->literals = NULL;
}

/* This must be called immediately before restoring the callout frame */
void
returnFromJNI(J9VMThread *currentThread, void *vbp)
{
	UDATA *bp = (UDATA*)vbp;
	J9SFJNINativeMethodFrame *frame = ((J9SFJNINativeMethodFrame*)(bp + 1)) - 1;
	UDATA flags = frame->specialFrameFlags;
	if (flags & J9_SSF_JNI_REFS_REDIRECTED) {
		freeStacks(currentThread, bp);
	}
	if (flags & J9_SSF_CALL_OUT_FRAME_ALLOC) {
		jniPopFrame(currentThread, JNIFRAME_TYPE_INTERNAL);
	}
}

#if defined(J9VM_ZOS_3164_INTEROPERABILITY)
void
queryJNIEnv31(J9VMThread* vmThread)
{
	/* Query libjvm31 to get a handle on the corresponding 31-bit JNIEnv struct
	 * for the given vmThread.
	 */
	UDATA slOpenResult = J9PORT_SL_FOUND;
	UDATA dllHandle = 0;
	UDATA getJNIEnv31Handle = 0;

	if (0 == vmThread->jniEnv31) {
		PORT_ACCESS_FROM_VMC(vmThread);
		slOpenResult = j9sl_open_shared_library("libjvm31.so", &dllHandle, (J9PORT_SLOPEN_OPEN_EXECUTABLE | OMRPORT_SLOPEN_ATTEMPT_31BIT_OPEN));

		if (J9PORT_SL_FOUND == slOpenResult) {
			if (0 == j9sl_lookup_name(dllHandle, "getJNIEnv31", &getJNIEnv31Handle, "L")) {
				ffi_type* args[1];
				void* values[1];
				UDATA returnValue = 0;
				ffi_cif cif_t;
				ffi_cif * const cif = &cif_t;

				args[0]= &ffi_type_sint64;
				values[0] = &vmThread;

				if (FFI_OK == ffi_prep_cif(cif, FFI_CEL4RO31, 1, &ffi_type_uint32, args)) {
					ffi_call(cif, FFI_FN(getJNIEnv31Handle), &returnValue, values);
				}
				vmThread->jniEnv31 = (U_32)returnValue;
			}
		}
		Assert_VM_true(0 != vmThread->jniEnv31);
	}
}

void
queryJavaVM31(J9JavaVM* vm)
{
	/* Query libjvm31 to get a handle on the corresponding 31-bit JavaVM struct
	 * for the given J9JavaVM.
	 */
	UDATA slOpenResult = J9PORT_SL_FOUND;
	UDATA dllHandle = 0;
	UDATA getJavaVM31Handle = 0;

	if (0 == vm->javaVM31) {
		PORT_ACCESS_FROM_JAVAVM(vm);
		slOpenResult = j9sl_open_shared_library("libjvm31.so", &dllHandle, (J9PORT_SLOPEN_OPEN_EXECUTABLE | OMRPORT_SLOPEN_ATTEMPT_31BIT_OPEN));

		if (J9PORT_SL_FOUND == slOpenResult) {
			if (0 == j9sl_lookup_name(dllHandle, "getJavaVM31", &getJavaVM31Handle, "L")) {
				ffi_type* args[1];
				void* values[1];
				UDATA returnValue = 0;
				ffi_cif cif_t;
				ffi_cif * const cif = &cif_t;

				args[0]= &ffi_type_sint64;
				values[0] = &vm;

				if (FFI_OK == ffi_prep_cif(cif, FFI_CEL4RO31, 1, &ffi_type_uint32, args)) {
					ffi_call(cif, FFI_FN(getJavaVM31Handle), &returnValue, values);
				}
				vm->javaVM31 = (U_32)returnValue;
			}
		}
		Assert_VM_true(0 != vm->javaVM31);
	}
}
#endif /* defined(J9VM_ZOS_3164_INTEROPERABILITY) */

} /* extern "C" */
