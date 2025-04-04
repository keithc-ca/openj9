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

#include "jvmtiHelpers.h"
#include "omrthread.h"
#include "omr.h"
#include "jvmti_internal.h"
#include "vmaccess.h"

jvmtiError JNICALL
jvmtiCreateRawMonitor(jvmtiEnv* env,
	const char* name,
	jrawMonitorID* monitor_ptr)
{
	jvmtiError rc;
	jrawMonitorID rv_monitor = NULL;

	Trc_JVMTI_jvmtiCreateRawMonitor_Entry(env, name);

	ENSURE_PHASE_ONLOAD_OR_LIVE(env);

	ENSURE_NON_NULL(name);
	ENSURE_NON_NULL(monitor_ptr);

	if (omrthread_monitor_init_with_name((omrthread_monitor_t *) &rv_monitor, J9THREAD_MONITOR_NAME_COPY, (char *)name) != 0) {
		JVMTI_ERROR(JVMTI_ERROR_OUT_OF_MEMORY);
	}
	rc = JVMTI_ERROR_NONE;

	Trc_JVMTI_rawMonitorCreated(rv_monitor);

done:
	if (NULL != monitor_ptr) {
		*monitor_ptr = rv_monitor;
	}
	TRACE_JVMTI_RETURN(jvmtiCreateRawMonitor);
}


jvmtiError JNICALL
jvmtiDestroyRawMonitor(jvmtiEnv* env,
	jrawMonitorID monitor)
{
	jvmtiError rc;

	Trc_JVMTI_jvmtiDestroyRawMonitor_Entry(env, monitor, omrthread_monitor_get_name((omrthread_monitor_t) monitor));

	ENSURE_PHASE_ONLOAD_OR_LIVE(env);

	ENSURE_MONITOR_NON_NULL(monitor);

	/* Make sure this thread does not own the monitor by releasing it until we get an error */

	while (omrthread_monitor_exit((omrthread_monitor_t) monitor) == 0) ;

	/* The destroy will fail if the monitor is in use */

	if (omrthread_monitor_destroy((omrthread_monitor_t) monitor) != 0) {
		JVMTI_ERROR(JVMTI_ERROR_NOT_MONITOR_OWNER);
	}
	rc = JVMTI_ERROR_NONE;

done:
	TRACE_JVMTI_RETURN(jvmtiDestroyRawMonitor);
}


jvmtiError JNICALL
jvmtiRawMonitorEnter(jvmtiEnv* env,
	jrawMonitorID monitor)
{
	J9JavaVM * vm = JAVAVM_FROM_ENV(env);
	jvmtiError rc;
	J9VMThread * currentThread;

	Trc_JVMTI_jvmtiRawMonitorEnter_Entry(env, monitor, omrthread_monitor_get_name((omrthread_monitor_t) monitor));

	ENSURE_MONITOR_NON_NULL(monitor);

	rc = getCurrentVMThread(vm, &currentThread);
	if (rc == JVMTI_ERROR_NONE) {
		/* JDK blocks here if the current thread is suspended - don't do VM access stuff if the current thread has exclusive */

		if (currentThread->publicFlags & J9_PUBLIC_FLAGS_HALT_THREAD_ANY) {
			if (currentThread->omrVMThread->exclusiveCount == 0) {
				/* Acquire VM access if the current thread does not already have it */
#if defined(J9VM_INTERP_ATOMIC_FREE_JNI)
				if (currentThread->inNative)
#else /* J9VM_INTERP_ATOMIC_FREE_JNI */
				if (0 == (currentThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS))
#endif /* J9VM_INTERP_ATOMIC_FREE_JNI */
				{
block:
					vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
					vm->internalVMFunctions->internalExitVMToJNI(currentThread);
				}
			}
		}

		if (0 == omrthread_monitor_enter((omrthread_monitor_t) monitor)) {
			/* CMVC 157789 : If the current thread is supposed to be blocked right now.
			 * release the monitor and block to simulate blocking before acquiring the
			 * monitor.
			 */
			if (currentThread->publicFlags & J9_PUBLIC_FLAGS_HALT_THREAD_ANY) {
				if (currentThread->omrVMThread->exclusiveCount == 0) {
#if defined(J9VM_INTERP_ATOMIC_FREE_JNI)
					if (currentThread->inNative)
#else /* J9VM_INTERP_ATOMIC_FREE_JNI */
					if (0 == (currentThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS))
#endif /* J9VM_INTERP_ATOMIC_FREE_JNI */
					{
						omrthread_monitor_exit((omrthread_monitor_t) monitor);
						goto block;
					}
				}
			}
#if JAVA_SPEC_VERSION >= 19
			if (UDATA_MAX != currentThread->continuationPinCount) {
				currentThread->continuationPinCount += 1;
			}
#endif /* JAVA_SPEC_VERSION >= 19 */
		} else {
#if defined(J9VM_INTERP_ATOMIC_FREE_JNI)
			if (currentThread->inNative)
#else /* J9VM_INTERP_ATOMIC_FREE_JNI */
			if (0 == (currentThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS))
#endif /* J9VM_INTERP_ATOMIC_FREE_JNI */
			{
				vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
				vm->internalVMFunctions->internalExitVMToJNI(currentThread);
			}
			rc = JVMTI_ERROR_INTERNAL;
		}
	}

done:
	TRACE_JVMTI_RETURN(jvmtiRawMonitorEnter);
}


jvmtiError JNICALL
jvmtiRawMonitorExit(jvmtiEnv* env,
	jrawMonitorID monitor)
{
	J9JavaVM * vm = JAVAVM_FROM_ENV(env);
	jvmtiError rc;
	J9VMThread * currentThread;

	Trc_JVMTI_jvmtiRawMonitorExit_Entry(env, monitor, omrthread_monitor_get_name((omrthread_monitor_t) monitor));

	ENSURE_MONITOR_NON_NULL(monitor);

	rc = getCurrentVMThread(vm, &currentThread);
	if (rc == JVMTI_ERROR_NONE) {
		if (omrthread_monitor_exit((omrthread_monitor_t) monitor) != 0) {
			rc = JVMTI_ERROR_NOT_MONITOR_OWNER;
#if JAVA_SPEC_VERSION >= 19
		} else {
			if (0 != currentThread->continuationPinCount) {
				currentThread->continuationPinCount -= 1;
			}
#endif /* JAVA_SPEC_VERSION >= 19 */
		}

		/* JDK blocks here if the current thread is suspended - don't do VM access stuff if the current thread has exclusive */

		if (currentThread->publicFlags & J9_PUBLIC_FLAGS_HALT_THREAD_ANY) {
			if (currentThread->omrVMThread->exclusiveCount == 0) {
				/* Acquire VM access if the current thread does not already have it */
#if defined(J9VM_INTERP_ATOMIC_FREE_JNI)
				if (currentThread->inNative)
#else /* J9VM_INTERP_ATOMIC_FREE_JNI */
				if (0 == (currentThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS))
#endif /* J9VM_INTERP_ATOMIC_FREE_JNI */
				{
					vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
					vm->internalVMFunctions->internalExitVMToJNI(currentThread);
				}
				/* There is still a timing hole here, since the thread may be suspended at this point */
			}
		}
	}

done:
	TRACE_JVMTI_RETURN(jvmtiRawMonitorExit);
}


jvmtiError JNICALL
jvmtiRawMonitorWait(jvmtiEnv *env,
	jrawMonitorID monitor,
	jlong millis)
{
	J9JavaVM *vm = JAVAVM_FROM_ENV(env);
	jvmtiError rc = JVMTI_ERROR_NONE;
	J9VMThread *currentThread = NULL;

	Trc_JVMTI_jvmtiRawMonitorWait_Entry(env, monitor, omrthread_monitor_get_name((omrthread_monitor_t) monitor));

	ENSURE_MONITOR_NON_NULL(monitor);

	rc = getCurrentVMThread(vm, &currentThread);
	if (JVMTI_ERROR_NONE == rc) {
		IDATA result = 0;
#if defined(J9VM_OPT_CRIU_SUPPORT)
		BOOLEAN waitTimed = (millis > 0);
		I_64 beforeWait = 0;
		if (waitTimed) {
			PORT_ACCESS_FROM_JAVAVM(vm);
			beforeWait = j9time_nano_time();
		}
continueWait:
#endif /* defined(J9VM_OPT_CRIU_SUPPORT) */
		millis = ((millis < 0) ? 0 : millis);
		result = omrthread_monitor_wait_interruptable((omrthread_monitor_t)monitor, millis, 0);
#if defined(J9VM_OPT_CRIU_SUPPORT)
		if (((J9THREAD_INTERRUPTED == result)
			|| (J9THREAD_PRIORITY_INTERRUPTED == result))
			&& J9_IS_SINGLE_THREAD_MODE(vm)
		) {
			if (waitTimed) {
				PORT_ACCESS_FROM_JAVAVM(vm);
				I_64 waitedTime = (j9time_nano_time() - beforeWait) / 1000000;
				if (waitedTime < millis) {
					millis -= waitedTime;
				} else {
					/* timed out, waiting another 10ms in single thread mode */
					millis = 10;
				}
			}
			/* continue waiting if interrupted spuriously in single thread mode */
			goto continueWait;
		}
#endif /* defined(J9VM_OPT_CRIU_SUPPORT) */
		switch (result) {
		case 0:
			/* FALLTHROUGH*/
		case J9THREAD_TIMED_OUT:
			rc = JVMTI_ERROR_NONE;
			break;
		case J9THREAD_ILLEGAL_MONITOR_STATE:
			rc = JVMTI_ERROR_NOT_MONITOR_OWNER;
			break;
		case J9THREAD_INTERRUPTED:
			/* FALLTHROUGH*/
		case J9THREAD_PRIORITY_INTERRUPTED: {
#if JAVA_SPEC_VERSION >= 19
			J9InternalVMFunctions const * const vmFuncs = vm->internalVMFunctions;
			j9object_t threadObject = NULL;
			j9object_t threadLock = NULL;

			vmFuncs->internalEnterVMFromJNI(currentThread);

			threadObject = currentThread->threadObject;
			Assert_JVMTI_true(NULL != threadObject);

			threadLock = J9VMJAVALANGTHREAD_LOCK(currentThread, threadObject);
			threadLock = (j9object_t)vmFuncs->objectMonitorEnter(currentThread, threadLock);

			if (J9_OBJECT_MONITOR_ENTER_FAILED(threadLock)) {
#if defined(J9VM_OPT_CRIU_SUPPORT)
				if (J9_OBJECT_MONITOR_CRIU_SINGLE_THREAD_MODE_THROW == (UDATA)threadLock) {
					vmFuncs->setCRIUSingleThreadModeJVMCRIUException(currentThread, 0, 0);
					rc = JVMTI_ERROR_INTERNAL;
				} else
#endif /* defined(J9VM_OPT_CRIU_SUPPORT) */
				if (J9_OBJECT_MONITOR_OOM == (UDATA)threadLock) {
					rc = JVMTI_ERROR_OUT_OF_MEMORY;
				}
				goto release;
			} else {
				/* Refetch J9VMThread->threadObject as it can be moved by the GC while waiting for the monitor. */
				threadObject = currentThread->threadObject;
				if (J9VMJAVALANGTHREAD_DEADINTERRUPT(currentThread, threadObject)) {
					J9VMJAVALANGTHREAD_SET_DEADINTERRUPT(currentThread, threadObject, JNI_FALSE);
					if (IS_JAVA_LANG_VIRTUALTHREAD(currentThread, threadObject)) {
						J9VMJAVALANGTHREAD_SET_DEADINTERRUPT(currentThread, currentThread->carrierThreadObject, JNI_FALSE);
					}
				}
				vmFuncs->objectMonitorExit(currentThread, threadLock);
			}

			rc = JVMTI_ERROR_INTERRUPT;
release:
			vmFuncs->internalExitVMToJNI(currentThread);
#else /* JAVA_SPEC_VERSION >= 19 */
			rc = JVMTI_ERROR_INTERRUPT;
#endif /* JAVA_SPEC_VERSION >= 19 */
			break;
		}
		case J9THREAD_INVALID_ARGUMENT:
			rc = JVMTI_ERROR_INTERRUPT;
			break;
		default:
			rc = JVMTI_ERROR_INTERNAL;
			break;
		}

		/* JDK blocks here if the current thread is suspended - don't do VM access stuff if the current thread has exclusive */
		if (currentThread->publicFlags & J9_PUBLIC_FLAGS_HALT_THREAD_ANY) {
			if (0 == currentThread->omrVMThread->exclusiveCount) {
				UDATA count = 0;

				while (0 == omrthread_monitor_exit((omrthread_monitor_t) monitor)) {
					++count;
				}

				/* Acquire VM access if the current thread does not already have it */
#if defined(J9VM_INTERP_ATOMIC_FREE_JNI)
				if (currentThread->inNative)
#else /* J9VM_INTERP_ATOMIC_FREE_JNI */
				if (0 == (currentThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS))
#endif /* J9VM_INTERP_ATOMIC_FREE_JNI */
				{
					vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
					vm->internalVMFunctions->internalExitVMToJNI(currentThread);
				}

				/* There is still a timing hole here, since the thread may be suspended at this point */
				while (0 != count--) {
					omrthread_monitor_enter((omrthread_monitor_t) monitor);
				}
			}
		}
	}

done:
	TRACE_JVMTI_RETURN(jvmtiRawMonitorWait);
}


jvmtiError JNICALL
jvmtiRawMonitorNotify(jvmtiEnv* env,
	jrawMonitorID monitor)
{
	jvmtiError rc;

	Trc_JVMTI_jvmtiRawMonitorNotify_Entry(env, monitor, omrthread_monitor_get_name((omrthread_monitor_t) monitor));

	ENSURE_MONITOR_NON_NULL(monitor);

	if (omrthread_monitor_notify((omrthread_monitor_t) monitor) == 0) {
		rc = JVMTI_ERROR_NONE;
	} else {
		rc = JVMTI_ERROR_NOT_MONITOR_OWNER;
	}

done:
	TRACE_JVMTI_RETURN(jvmtiRawMonitorNotify);
}


jvmtiError JNICALL
jvmtiRawMonitorNotifyAll(jvmtiEnv* env,
	jrawMonitorID monitor)
{
	jvmtiError rc;

	Trc_JVMTI_jvmtiRawMonitorNotifyAll_Entry(env, monitor, omrthread_monitor_get_name((omrthread_monitor_t) monitor));

	ENSURE_MONITOR_NON_NULL(monitor);

	if (omrthread_monitor_notify_all((omrthread_monitor_t) monitor) == 0) {
		rc = JVMTI_ERROR_NONE;
	} else {
		rc = JVMTI_ERROR_NOT_MONITOR_OWNER;
	}

done:
	TRACE_JVMTI_RETURN(jvmtiRawMonitorNotifyAll);
}
