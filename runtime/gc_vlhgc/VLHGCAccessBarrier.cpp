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


/**
 * @file
 * @ingroup GC_Modron_Base
 */

#include "j9.h"
#include "j9cfg.h"
#include "j9consts.h"
#include "j9protos.h"
#include "ModronAssertions.h"

#include "VLHGCAccessBarrier.hpp"
#include "AtomicOperations.hpp"
#include "CardTable.hpp"
#include "Debug.hpp"
#include "EnvironmentVLHGC.hpp"
#include "mmhook_internal.h"
#include "GCExtensions.hpp"
#include "HeapRegionManager.hpp"
#include "IncrementalGenerationalGC.hpp"
#include "JNICriticalRegion.hpp"
#include "ObjectModel.hpp"
#include "SublistFragment.hpp"
#include "ForwardedHeader.hpp"
#include "RootScanner.hpp"

MM_VLHGCAccessBarrier *
MM_VLHGCAccessBarrier::newInstance(MM_EnvironmentBase *env)
{
	MM_VLHGCAccessBarrier *barrier;
	
	barrier = (MM_VLHGCAccessBarrier *)env->getForge()->allocate(sizeof(MM_VLHGCAccessBarrier), MM_AllocationCategory::FIXED, J9_GET_CALLSITE());
	if (barrier) {
		new(barrier) MM_VLHGCAccessBarrier(env);
		if (!barrier->initialize(env)) {
			barrier->kill(env);
			barrier = NULL;
		}
	}
	return barrier;
}

bool 
MM_VLHGCAccessBarrier::initialize(MM_EnvironmentBase *env)
{
	return MM_ObjectAccessBarrier::initialize(env);
}

void 
MM_VLHGCAccessBarrier::kill(MM_EnvironmentBase *env)
{
	tearDown(env);
	env->getForge()->free(this);
}

void
MM_VLHGCAccessBarrier::tearDown(MM_EnvironmentBase *env)
{
	MM_ObjectAccessBarrier::tearDown(env);
}

/**
 * Called after an object is stored into another object.
 */
void
MM_VLHGCAccessBarrier::postObjectStore(J9VMThread *vmThread, J9Object *destObject, fj9object_t *destAddress, J9Object *value, bool isVolatile)
{
	postObjectStoreImpl(vmThread, destObject, value);
}

/**
 * Called after an object is stored into a class.
 */
void
MM_VLHGCAccessBarrier::postObjectStore(J9VMThread *vmThread, J9Class *destClass, J9Object **destAddress, J9Object *value, bool isVolatile)
{
	j9object_t destObject = J9VM_J9CLASS_TO_HEAPCLASS(destClass);

	/* destObject is guaranteed to be in old space, so the common code path will remember objects appropriately here */
	postObjectStoreImpl(vmThread, destObject, value);
}

bool 
MM_VLHGCAccessBarrier::postBatchObjectStore(J9VMThread *vmThread, J9Object *destObject, bool isVolatile)
{
	postBatchObjectStoreImpl(vmThread, destObject);
	
	return true;
}

bool 
MM_VLHGCAccessBarrier::postBatchObjectStore(J9VMThread *vmThread, J9Class *destClass, bool isVolatile)
{
	j9object_t destObject = J9VM_J9CLASS_TO_HEAPCLASS(destClass);
	
	postBatchObjectStoreImpl(vmThread, destObject);
	
	return true;
}

/**
 * Generational write barrier call when a single object is stored into another.
 * The remembered set system consists of a physical list of objects in the OLD area that
 * may contain references to the new area.  The mutator is responsible for adding these old
 * area objects to the remembered set; the collectors are responsible for removing these objects
 * from the list when they no longer contain references.  Objects that are to be remembered have their
 * REMEMBERED bit set in the flags field.  For performance reasons, sublists are used to maintain the
 * remembered set.
 * 
 * @param vmThread The current thread that has performed the store.
 * @param dstObject The object which is being stored into.
 * @param srcObject The object being stored.
 * 
 * @note The write barrier can be called with minimal, all, or no validation checking.
 * @note Any object that contains a new reference MUST have its REMEMBERED bit set.
 */
void
MM_VLHGCAccessBarrier::postObjectStoreImpl(J9VMThread *vmThread, J9Object *dstObject, J9Object *srcObject)
{
	/* If the source object is NULL, there is no need for a write barrier. */
	if(NULL != srcObject) {
		MM_EnvironmentVLHGC *env = MM_EnvironmentVLHGC::getEnvironment(vmThread);
		_extensions->cardTable->dirtyCard(env, dstObject);
	}
}

/**
 * Generational write barrier call when a group of objects are stored into a single object.
 * The remembered set system consists of a physical list of objects in the OLD area that
 * may contain references to the new area.  The mutator is responsible for adding these old
 * area objects to the remembered set; the collectors are responsible for removing these objects
 * from the list when they no longer contain references.  Objects that are to be remembered have their
 * REMEMBERED bit set in the flags field.  For performance reasons, sublists are used to maintain the
 * remembered set.
 * 
 * @param vmThread The current thread that has performed the store.
 * @param dstObject The object which is being stored into.
 * 
 * @note The write barrier can be called with minimal, all, or no validation checking.
 * @note Any object that contains a new reference MUST have its REMEMBERED bit set.
 * @note This call is typically used by array copies, when it may be more efficient
 * to optimistically add an object to the remembered set without checking too hard.
 */
void 
MM_VLHGCAccessBarrier::postBatchObjectStoreImpl(J9VMThread *vmThread, J9Object *dstObject)
{
	MM_EnvironmentVLHGC *env = MM_EnvironmentVLHGC::getEnvironment(vmThread);
	_extensions->cardTable->dirtyCard(env, dstObject);
}


/**
 * Finds opportunities for doing the copy without or partially executing writeBarrier.
 * @return ARRAY_COPY_SUCCESSFUL if copy was successful, ARRAY_COPY_NOT_DONE no copy is done
 */
I_32
MM_VLHGCAccessBarrier::backwardReferenceArrayCopyIndex(J9VMThread *vmThread, J9IndexableObject *srcObject, J9IndexableObject *destObject, I_32 srcIndex, I_32 destIndex, I_32 lengthInSlots)
{
	MM_EnvironmentVLHGC *env = MM_EnvironmentVLHGC::getEnvironment(vmThread);
	I_32 retValue = ARRAY_COPY_NOT_DONE;
	
	/* a high level caller ensured destObject == srcObject */
	Assert_MM_true(destObject == srcObject);
	if (_extensions->indexableObjectModel.isInlineContiguousArraylet(destObject)) {
		 retValue = doCopyContiguousBackward(vmThread, srcObject, destObject, srcIndex, destIndex, lengthInSlots);

		Assert_MM_true(retValue == ARRAY_COPY_SUCCESSFUL);
		if (((MM_IncrementalGenerationalGC *)_extensions->getGlobalCollector())->isGlobalMarkPhaseRunning()) {
			_extensions->cardTable->dirtyCard(env, (J9Object *)destObject);
		}
	}
	
	return retValue;
}

/**
 * Finds opportunities for doing the copy without or partially executing writeBarrier.
 * @return ARRAY_COPY_SUCCESSFUL if copy was successful, ARRAY_COPY_NOT_DONE no copy is done
 */
I_32
MM_VLHGCAccessBarrier::forwardReferenceArrayCopyIndex(J9VMThread *vmThread, J9IndexableObject *srcObject, J9IndexableObject *destObject, I_32 srcIndex, I_32 destIndex, I_32 lengthInSlots)
{
	MM_EnvironmentVLHGC *env = MM_EnvironmentVLHGC::getEnvironment(vmThread);
	I_32 retValue = ARRAY_COPY_NOT_DONE;

	if (_extensions->indexableObjectModel.isInlineContiguousArraylet(destObject) && _extensions->indexableObjectModel.isInlineContiguousArraylet(srcObject)) {
		retValue = doCopyContiguousForward(vmThread, srcObject, destObject, srcIndex, destIndex, lengthInSlots);

		Assert_MM_true(retValue == ARRAY_COPY_SUCCESSFUL);
		if ((destObject != srcObject) || ((MM_IncrementalGenerationalGC *)_extensions->getGlobalCollector())->isGlobalMarkPhaseRunning()) {
			_extensions->cardTable->dirtyCard(env, (J9Object *)destObject);
		}

	}
	
	return retValue;
}

/**
 * VMDESIGN 2048
 * Special barrier for auto-remembering stack-referenced objects. This must be called 
 * in two cases:
 * 1) an object which was allocated directly into old space.
 * 2) an object which is being constructed via JNI
 * 
 * @param vmThread[in] the current thread
 * @param object[in] the object to be remembered 
 */
void 
MM_VLHGCAccessBarrier::recentlyAllocatedObject(J9VMThread *vmThread, J9Object *dstObject) 
{ 
	/* TODO: we may need to revive this if we apply VMDESIGN 2048 to Tarok specs */
}

void 
MM_VLHGCAccessBarrier::postStoreClassToClassLoader(J9VMThread *vmThread, J9ClassLoader *destClassLoader, J9Class *srcClass)
{
	J9Object *classLoaderObject = destClassLoader->classLoaderObject;
	/* unlikely, but during bootstrap the classLoaderObject can be NULL until j.l.ClassLoader is available */
	if (NULL == classLoaderObject) {
		/* only bootstrap classes can appear here, and it's safe to miss barriers for them since they're roots */
		Assert_MM_true(srcClass->classLoader == vmThread->javaVM->systemClassLoader);
	} else {
		J9Object *classObject = J9VM_J9CLASS_TO_HEAPCLASS(srcClass);
		postObjectStoreImpl(vmThread, classLoaderObject, classObject);
	}
}

void
MM_VLHGCAccessBarrier::postStoreModuleToClassLoader(J9VMThread *vmThread, J9ClassLoader *destClassLoader, J9Module *srcModule)
{
	J9Object *classLoaderObject = destClassLoader->classLoaderObject;
	/* unlikely, but during bootstrap the classLoaderObject can be NULL until j.l.ClassLoader is available */
	if (NULL == classLoaderObject) {
		/* only persistent class loaders can appear here, and it's safe to miss barriers for them since they're roots */
		Assert_MM_true(
				(destClassLoader == vmThread->javaVM->systemClassLoader)
				|| (destClassLoader == vmThread->javaVM->applicationClassLoader)
				|| (destClassLoader == vmThread->javaVM->extensionClassLoader));
	} else {
		J9Object *moduleObject = srcModule->moduleObject;
		postObjectStoreImpl(vmThread, classLoaderObject, moduleObject);
	}
}

IDATA
MM_VLHGCAccessBarrier::indexableDataDisplacement(J9StackWalkState *walkState, J9IndexableObject *src, J9IndexableObject *dst)
{
	IDATA displacement = 0;

#if defined(J9VM_ENV_DATA64)
	MM_EnvironmentBase *env = MM_EnvironmentBase::getEnvironment(walkState->currentThread->omrVMThread);
	Assert_MM_true(_extensions->isVirtualLargeObjectHeapEnabled);
	/* Potential danger in future of this not being called by a GC Stack Walker - hence this assert */
	Assert_MM_true(walkState->objectSlotWalkFunction == gc_vmThreadStackDoOSlotIterator);

	/* When checking adjacency, we pass both src and dst address, since depending on movement type (evacuate vs sliding) only one is safe to use.
	 * If we implement concurrent copy-forward, which will require copying of dataAddr before forwarding,
	 * consider this simplifying to always do adjacency against dst.
	 */
	MM_HeapRegionManager *regionManager = _extensions->getHeap()->getHeapRegionManager();
	MM_HeapRegionDescriptorVLHGC *srcRegion = (MM_HeapRegionDescriptorVLHGC *)regionManager->regionDescriptorForAddress(src);

	J9IndexableObject *objectToCheckAdjacency = NULL;

	if (srcRegion->_compactData._shouldCompact) {
		Assert_GC_true_with_message(env, !srcRegion->_copyForwardData._evacuateSet, "Evac set for compact src region %p src obj %p dst obj%p\n", srcRegion, src, dst);
		/* Moved by sliding compact - source may be overwritten. */
		objectToCheckAdjacency = dst;
	} else if (srcRegion->_copyForwardData._evacuateSet) {
		/* Moved (or still being moved) by copy-forward - destination may not be fully copied yet. */
		objectToCheckAdjacency = src;
	} else {
		Assert_GC_true_with_message(env, false, "Neither evac nor compact set src region %p src obj %p dst obj%p\n", srcRegion, src, dst);
	}

	if (_extensions->indexableObjectModel.isDataAdjacentToHeader(objectToCheckAdjacency))
#endif /* defined(J9VM_ENV_DATA64) */
	{
		displacement = MM_ObjectAccessBarrier::indexableDataDisplacement(walkState, src, dst);
	}
	return displacement;
}


void *
MM_VLHGCAccessBarrier::jniGetPrimitiveArrayCritical(J9VMThread *vmThread, jarray array, jboolean *isCopy)
{
	void *data = NULL;
	/* Set default isCopy value to JNI_FALSE, where in case we need to copy array critical
	 * the value is set to JNI_TRUE in copyArrayCritical()
	 */
	if (NULL != isCopy) {
		*isCopy = JNI_FALSE;
	}

	if (!_extensions->indexableObjectModel.isVirtualLargeObjectHeapEnabled()) {
		return jniGetPrimitiveArrayCriticalForArraylet(vmThread, array, isCopy);
	}

	bool alwaysCopyInCritical = (vmThread->javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL;
	if (alwaysCopyInCritical) {
		VM_VMAccess::inlineEnterVMFromJNI(vmThread);
		J9IndexableObject *arrayObject = (J9IndexableObject *)J9_JNI_UNWRAP_REFERENCE(array);
		copyArrayCritical(vmThread, &data, arrayObject, isCopy);
		VM_VMAccess::inlineExitVMToJNI(vmThread);
	} else {
		/* no need to check isInlineContiguousArraylet(), if VirtualLargeObjectHeapEnabled all of array are InlineContiguous except 0 size array, deal 0-size array as adjacent array */
		/* off heap enabled and Adjacent or not adjacent, acquire access to prevent object being moved by GC access and return a direct pointer.
		 */
		MM_JNICriticalRegion::enterCriticalRegion(vmThread, false);
		J9IndexableObject *arrayObject = (J9IndexableObject *)J9_JNI_UNWRAP_REFERENCE(array);
		data = (void *)_extensions->indexableObjectModel.getDataPointerForContiguous(arrayObject);

		if (NULL == data) {
			/* For 0-sized array, can't return plain dataAddr value, since it's NULL, and the caller is supposed to treat it as an error.
			 * Return the end of the header, instead.
			 */
			data = (void *)((uintptr_t) arrayObject + _extensions->indexableObjectModel.contiguousIndexableHeaderSize());
		}
		/* we need to increment this region's critical count so that we know not to compact it */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(arrayObject))->_criticalRegionsInUse);
		MM_AtomicOperations::add(criticalCount, 1);
	}
	return data;
}

void *
MM_VLHGCAccessBarrier::jniGetPrimitiveArrayCriticalForArraylet(J9VMThread *vmThread, jarray array, jboolean *isCopy)
{
	void *data = NULL;
	GC_ArrayObjectModel *indexableObjectModel = &_extensions->indexableObjectModel;

	VM_VMAccess::inlineEnterVMFromJNI(vmThread);

	J9IndexableObject *arrayObject = (J9IndexableObject *)J9_JNI_UNWRAP_REFERENCE(array);
	bool alwaysCopyInCritical = (vmThread->javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL;

	if (alwaysCopyInCritical || !indexableObjectModel->isInlineContiguousArraylet(arrayObject)) {
		/* alwaysCopyInCritical or discontiguous (including 0 size array) */
		copyArrayCritical(vmThread, &data, arrayObject, isCopy);
	} else {
		/* contiguous for offheap disabled.
		   acquire access to prevent object being moved by GC access and return a direct pointer. */
		MM_JNICriticalRegion::enterCriticalRegion(vmThread, true);
		Assert_MM_true(vmThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS);
		arrayObject = (J9IndexableObject *)J9_JNI_UNWRAP_REFERENCE(array);
		data = (void *)indexableObjectModel->getDataPointerForContiguous(arrayObject);
		/* we need to increment this region's critical count so that we know not to compact it */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(arrayObject))->_criticalRegionsInUse);
		MM_AtomicOperations::add(criticalCount, 1);
	}
	VM_VMAccess::inlineExitVMToJNI(vmThread);
	return data;
}

void
MM_VLHGCAccessBarrier::jniReleasePrimitiveArrayCritical(J9VMThread *vmThread, jarray array, void *elems, jint mode)
{
	if (!_extensions->indexableObjectModel.isVirtualLargeObjectHeapEnabled()) {
		jniReleasePrimitiveArrayCriticalForArraylet(vmThread, array, elems, mode);
		return;
	}
	bool alwaysCopyInCritical = (vmThread->javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL;
	if (alwaysCopyInCritical) {
		VM_VMAccess::inlineEnterVMFromJNI(vmThread);
		J9IndexableObject *arrayObject = (J9IndexableObject *)J9_JNI_UNWRAP_REFERENCE(array);
		copyBackArrayCritical(vmThread, elems, &arrayObject, mode);
		VM_VMAccess::inlineExitVMToJNI(vmThread);
	} else {
		/* no need to check isInlineContiguousArraylet(), if VirtualLargeObjectHeapEnabled all of array are InlineContiguous except 0-size array,
		 * for 0-size array(data == NULL) logs an error trace.
		 */
		J9IndexableObject *arrayObject = (J9IndexableObject *)J9_JNI_UNWRAP_REFERENCE(array);
		void *data = (void *)_extensions->indexableObjectModel.getDataPointerForContiguous(arrayObject);
		if (elems != data) {
			Trc_MM_JNIReleasePrimitiveArrayCritical_invalid(vmThread, arrayObject, elems, data);
		}

		/* we need to decrement this region's critical count */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(arrayObject))->_criticalRegionsInUse);
		Assert_MM_true((*criticalCount) > 0);
		MM_AtomicOperations::subtract(criticalCount, 1);
		MM_JNICriticalRegion::exitCriticalRegion(vmThread, false);
	}
}

void
MM_VLHGCAccessBarrier::jniReleasePrimitiveArrayCriticalForArraylet(J9VMThread *vmThread, jarray array, void *elems, jint mode)
{
	GC_ArrayObjectModel *indexableObjectModel = &_extensions->indexableObjectModel;

	VM_VMAccess::inlineEnterVMFromJNI(vmThread);

	J9IndexableObject *arrayObject = (J9IndexableObject *)J9_JNI_UNWRAP_REFERENCE(array);
	bool alwaysCopyInCritical = (vmThread->javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL;

	if (alwaysCopyInCritical || !indexableObjectModel->isInlineContiguousArraylet(arrayObject)) {
		/* alwaysCopyInCritical or discontiguous (including 0 size array) */
		copyBackArrayCritical(vmThread, elems, &arrayObject, mode);
	} else {
		/* adjacent for offheap enabled and contiguous for offheap disabled,
		 * Objects can not be moved if critical section is active
		 * This trace point will be generated if object has been moved or passed value of elems is corrupted
		 */
		void *data = (void *)indexableObjectModel->getDataPointerForContiguous(arrayObject);
		if(elems != data) {
			Trc_MM_JNIReleasePrimitiveArrayCritical_invalid(vmThread, arrayObject, elems, data);
		}
		/* we need to decrement this region's critical count */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(arrayObject))->_criticalRegionsInUse);
		Assert_MM_true((*criticalCount) > 0);
		MM_AtomicOperations::subtract(criticalCount, 1);
		MM_JNICriticalRegion::exitCriticalRegion(vmThread, true);
	}
	VM_VMAccess::inlineExitVMToJNI(vmThread);
}

const jchar *
MM_VLHGCAccessBarrier::jniGetStringCritical(J9VMThread *vmThread, jstring str, jboolean *isCopy)
{
	jchar *data = NULL;
	if (NULL != isCopy) {
		*isCopy = JNI_FALSE;
	}
	if (!_extensions->indexableObjectModel.isVirtualLargeObjectHeapEnabled()) {
		return jniGetStringCriticalForArraylet(vmThread, str, isCopy);
	}
	J9JavaVM *javaVM = vmThread->javaVM;
	bool isCompressed = false;
	bool shouldCopy = false;
	bool hasVMAccess = false;

	if ((javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) {
		VM_VMAccess::inlineEnterVMFromJNI(vmThread);
		hasVMAccess = true;
		shouldCopy = true;
	} else if (IS_STRING_COMPRESSION_ENABLED_VM(javaVM)) {
		/* If the string bytes are in compressed UNICODE, then we need to copy to decompress */
		VM_VMAccess::inlineEnterVMFromJNI(vmThread);
		hasVMAccess = true;
		J9Object *stringObject = (J9Object *)J9_JNI_UNWRAP_REFERENCE(str);
		if (IS_STRING_COMPRESSED(vmThread,stringObject)) {
			isCompressed = true;
			shouldCopy = true;
		}
	}

	if (shouldCopy) {
		J9Object *stringObject = (J9Object *)J9_JNI_UNWRAP_REFERENCE(str);
		J9IndexableObject *valueObject = (J9IndexableObject*)J9VMJAVALANGSTRING_VALUE(vmThread, stringObject);

		if (IS_STRING_COMPRESSED(vmThread, stringObject)) {
				isCompressed = true;
		}

		copyStringCritical(vmThread, &data, valueObject, stringObject, isCopy, isCompressed);
	} else {
		/* no need to check isInlineContiguousArraylet(), if VirtualLargeObjectHeapEnabled all of array are InlineContiguous except 0-size array,
		 * deal 0-size array as adjacent array.
		 */
		/* Acquire access and return a direct pointer. */
		MM_JNICriticalRegion::enterCriticalRegion(vmThread, hasVMAccess);
		J9Object *stringObject = (J9Object *)J9_JNI_UNWRAP_REFERENCE(str);
		J9IndexableObject *valueObject = (J9IndexableObject *)J9VMJAVALANGSTRING_VALUE(vmThread, stringObject);

		data = (jchar *)_extensions->indexableObjectModel.getDataPointerForContiguous(valueObject);

		if (NULL == data) {
			/* For 0-sized array, can't return plain dataAddr value, since it's NULL, and the caller is supposed to treat it as an error.
			 * Return the end of the header, instead.
			 */
			data = (jchar *) ((uintptr_t) valueObject + _extensions->indexableObjectModel.contiguousIndexableHeaderSize());
		}

		/* we need to increment this region's critical count so that we know not to compact it */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(valueObject))->_criticalRegionsInUse);
		MM_AtomicOperations::add(criticalCount, 1);
	}

	if (hasVMAccess) {
		VM_VMAccess::inlineExitVMToJNI(vmThread);
	}
	return data;
}

const jchar *
MM_VLHGCAccessBarrier::jniGetStringCriticalForArraylet(J9VMThread *vmThread, jstring str, jboolean *isCopy)
{
	jchar *data = NULL;
	GC_ArrayObjectModel *indexableObjectModel = &_extensions->indexableObjectModel;

	J9JavaVM *javaVM = vmThread->javaVM;
	VM_VMAccess::inlineEnterVMFromJNI(vmThread);

	J9Object *stringObject = (J9Object *)J9_JNI_UNWRAP_REFERENCE(str);
	J9IndexableObject *valueObject = (J9IndexableObject *)J9VMJAVALANGSTRING_VALUE(vmThread, stringObject);
	/* If the string bytes are in compressed UNICODE, then we need to copy to decompress */	
	bool isCompressed = IS_STRING_COMPRESSION_ENABLED_VM(javaVM) && IS_STRING_COMPRESSED(vmThread, stringObject);

	bool alwaysCopyInCritical = (javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL;
	if (alwaysCopyInCritical || isCompressed || !indexableObjectModel->isInlineContiguousArraylet(valueObject)) {
		/*  alwaysCopyInCritical or isCompressed  or discontiguous (including 0 size array) */
		copyStringCritical(vmThread, &data, valueObject, stringObject, isCopy, isCompressed);
	} else {
		/**
		 *  contiguous for offheap disabled
		 *  acquire access to prevent object being moved by GC access and return a direct pointer
		 */
		MM_JNICriticalRegion::enterCriticalRegion(vmThread, true);
		Assert_MM_true(vmThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS);
		data = (jchar *)_extensions->indexableObjectModel.getDataPointerForContiguous(valueObject);

		/* we need to increment this region's critical count so that we know not to compact it */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(valueObject))->_criticalRegionsInUse);
		MM_AtomicOperations::add(criticalCount, 1);
	}

	VM_VMAccess::inlineExitVMToJNI(vmThread);
	return data;
}

void
MM_VLHGCAccessBarrier::jniReleaseStringCritical(J9VMThread *vmThread, jstring str, const jchar *elems)
{
	if (!_extensions->indexableObjectModel.isVirtualLargeObjectHeapEnabled()) {
		jniReleaseStringCriticalForArraylet(vmThread, str, elems);
		return;
	}
	J9JavaVM *javaVM = vmThread->javaVM;
	bool hasVMAccess = false;
	bool shouldCopy = false;

	if ((javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) {
		shouldCopy = true;
	} else if (IS_STRING_COMPRESSION_ENABLED_VM(javaVM)) {
		VM_VMAccess::inlineEnterVMFromJNI(vmThread);
		hasVMAccess = true;
		J9Object *stringObject = (J9Object *)J9_JNI_UNWRAP_REFERENCE(str);
		if (IS_STRING_COMPRESSED(vmThread, stringObject)) {
			shouldCopy = true;
		}
	}

	if (shouldCopy) {
		freeStringCritical(vmThread, elems);
	} else {
		J9Object *stringObject = (J9Object *)J9_JNI_UNWRAP_REFERENCE(str);
		J9IndexableObject *valueObject = (J9IndexableObject *)J9VMJAVALANGSTRING_VALUE(vmThread, stringObject);

		/* we need to decrement this region's critical count */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(valueObject))->_criticalRegionsInUse);
		Assert_MM_true((*criticalCount) > 0);
		MM_AtomicOperations::subtract(criticalCount, 1);
		MM_JNICriticalRegion::exitCriticalRegion(vmThread, hasVMAccess);
	}
	if (hasVMAccess) {
		VM_VMAccess::inlineExitVMToJNI(vmThread);
	}
}

void
MM_VLHGCAccessBarrier::jniReleaseStringCriticalForArraylet(J9VMThread *vmThread, jstring str, const jchar *elems)
{
	J9JavaVM *javaVM = vmThread->javaVM;

	GC_ArrayObjectModel *indexableObjectModel = &_extensions->indexableObjectModel;

	VM_VMAccess::inlineEnterVMFromJNI(vmThread);

	J9Object *stringObject = (J9Object *)J9_JNI_UNWRAP_REFERENCE(str);
	J9IndexableObject *valueObject = (J9IndexableObject *)J9VMJAVALANGSTRING_VALUE(vmThread, stringObject);

	bool alwaysCopyInCritical = (javaVM->runtimeFlags & J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL) == J9_RUNTIME_ALWAYS_COPY_JNI_CRITICAL;
	bool isCompressed = IS_STRING_COMPRESSION_ENABLED_VM(javaVM) && IS_STRING_COMPRESSED(vmThread, stringObject);

	if (alwaysCopyInCritical || isCompressed || !indexableObjectModel->isInlineContiguousArraylet(valueObject)) {
		/*  alwaysCopyInCritical or isCompressed  or discontiguous (including 0 size array) */
		freeStringCritical(vmThread, elems);
	} else {
		/**
		 *  contiguous for offheap disabled,
		 *  direct pointer, just drop access
		 */
		/* we need to decrement this region's critical count */
		UDATA volatile *criticalCount = &(((MM_HeapRegionDescriptorVLHGC *)_heap->getHeapRegionManager()->regionDescriptorForAddress(valueObject))->_criticalRegionsInUse);
		Assert_MM_true((*criticalCount) > 0);
		MM_AtomicOperations::subtract(criticalCount, 1);
		MM_JNICriticalRegion::exitCriticalRegion(vmThread, true);
	}
	VM_VMAccess::inlineExitVMToJNI(vmThread);
}

bool
MM_VLHGCAccessBarrier::preWeakRootSlotRead(J9VMThread *vmThread, j9object_t *srcAddress)
{
	MM_ForwardedHeader forwardedHeader(*srcAddress, compressObjectReferences());
	J9Object *forwardedPtr = forwardedHeader.getForwardedObject();
	if (NULL != forwardedPtr) {
		*srcAddress = forwardedPtr;
	}

	return true;
}

bool
MM_VLHGCAccessBarrier::preWeakRootSlotRead(J9JavaVM *vm, j9object_t *srcAddress)
{
	MM_ForwardedHeader forwardedHeader(*srcAddress, compressObjectReferences());
	J9Object *forwardedPtr = forwardedHeader.getForwardedObject();
	if (NULL != forwardedPtr) {
		*srcAddress = forwardedPtr;
	}

	return true;
}

void
MM_VLHGCAccessBarrier::postUnmountContinuation(J9VMThread *vmThread, j9object_t contObject)
{
	/* Conservatively assume that via mutations of stack slots (which are not subject to access barriers),
	 * all post-write barriers have been triggered on this Continuation object, since it's been mounted.
	 */
	postBatchObjectStore(vmThread, contObject);
}
