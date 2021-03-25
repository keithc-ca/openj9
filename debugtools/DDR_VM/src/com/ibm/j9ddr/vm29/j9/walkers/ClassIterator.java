/*******************************************************************************
 * Copyright (c) 1991, 2021 IBM Corp. and others
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
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/
package com.ibm.j9ddr.vm29.j9.walkers;

import java.util.Iterator;
import java.util.NoSuchElementException;
import java.util.logging.Logger;

import com.ibm.j9ddr.CorruptDataException;
import com.ibm.j9ddr.logging.LoggerNames;
import com.ibm.j9ddr.vm29.j9.AlgorithmVersion;
import com.ibm.j9ddr.vm29.j9.Pool;
import com.ibm.j9ddr.vm29.pointer.generated.J9ClassLoaderPointer;
import com.ibm.j9ddr.vm29.pointer.generated.J9ClassPointer;
import com.ibm.j9ddr.vm29.pointer.generated.J9HashTablePointer;
import com.ibm.j9ddr.vm29.pointer.generated.J9PoolPointer;
import com.ibm.j9ddr.vm29.structure.VmInternalConstants;

//iterator for
//TODO handle J9Package_ID_TAG check

public class ClassIterator implements Iterator<J9ClassPointer> {

	private final Iterator<J9ClassPointer> iterator;

	private J9ClassPointer next;

	protected ClassIterator(Iterator<J9ClassPointer> iterator) {
		this.iterator = iterator;
		setNextItem();
	}

	public static Iterator<J9ClassPointer> fromJ9Classloader(J9ClassLoaderPointer loader) throws CorruptDataException {
		J9HashTablePointer table = loader.classHashTable();
		J9PoolPointer listNodePool = table.listNodePool();
		Iterator<J9ClassPointer> iterator;
		if (listNodePool.notNull()) {
			iterator = Pool.fromJ9Pool(listNodePool, J9ClassPointer.class, false).iterator();
		} else {
			iterator = new ArrayIterator<>(J9ClassPointer.class, table.tableSize().intValue(), table.nodes());
		}
		return new ClassIterator(iterator);
	}

	@Override
	public boolean hasNext() {
		return next.notNull();
	}

	private void setNextItem() {
		while (iterator.hasNext()) {
			next = iterator.next();
			if ((next != null) && next.notNull()) {
				if ((next.getAddress() & VmInternalConstants.MASK_RAM_CLASS) == VmInternalConstants.TAG_RAM_CLASS) {
					return; // found a RAM class
				}
			}
		}
		next = J9ClassPointer.NULL;
	}

	@Override
	public J9ClassPointer next() {
		if (hasNext()) {
			J9ClassPointer ptr = next;
			setNextItem();
			return ptr;
		}
		throw new NoSuchElementException();
	}

	@Override
	public void remove() {
		throw new UnsupportedOperationException();
	}

}
