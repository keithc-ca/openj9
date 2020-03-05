/*******************************************************************************
 * Copyright (c) 2009, 2020 IBM Corp. and others
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
package com.ibm.j9ddr.corereaders.memory;

import com.ibm.j9ddr.AddressedCorruptDataException;

/**
 * Exception class representing a memory fault (GPF)
 *
 * @author andhall
 */
public class MemoryFault extends AddressedCorruptDataException {

	private static final long serialVersionUID = 4274925376210643492L;

	private final String message;

	/**
	 * @param address
	 *            Address that caused fault
	 */
	public MemoryFault(long address) {
		this(address, null, null);
	}

	/**
	 * @param address
	 *            Address that caused fault
	 * @param message
	 *            Exception message
	 */
	public MemoryFault(long address, String message) {
		this(address, message, null);
	}

	/**
	 * @param address
	 *            Address that caused fault
	 * @param cause
	 *            Underlying throwable that caused memory fault
	 */
	public MemoryFault(long address, Throwable cause) {
		this(address, null, cause);
	}

	/**
	 * @param address
	 *            Address that caused fault
	 * @param message
	 *            Exception message
	 * @param cause
	 *            Underlying throwable that caused memory fault
	 */
	public MemoryFault(long address, String message, Throwable cause) {
		super(address, cause);
		this.message = message;
	}

	/* Creating the message up front is quite expensive, particularly if
	 * it's then caught and ignored so generate the message when it's
	 * requested.
	 * @see java.lang.Throwable#getMessage()
	 */
	@Override
	public String getMessage() {
		if (message != null) {
			return String.format("Memory Fault reading 0x%08X : %s", address, message);
		} else {
			return String.format("Memory Fault reading 0x%08X", address);
		}
	}

}
