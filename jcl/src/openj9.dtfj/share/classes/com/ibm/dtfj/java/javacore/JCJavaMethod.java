/*[INCLUDE-IF Sidecar18-SE]*/
/*
 * Copyright IBM Corp. and others 2007
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
 */
package com.ibm.dtfj.java.javacore;

import java.util.Collections;
import java.util.Iterator;

import com.ibm.dtfj.image.CorruptDataException;
import com.ibm.dtfj.image.DataUnavailable;
import com.ibm.dtfj.image.javacore.JCCorruptData;
import com.ibm.dtfj.java.JavaClass;
import com.ibm.dtfj.java.JavaMethod;

public class JCJavaMethod implements JavaMethod {

	private final String fName;
	private final JavaClass fJavaClass;

	private String fSignature;

	public JCJavaMethod (String name, JCJavaClass javaClass) throws JCInvalidArgumentsException{
		if (name == null) {
			throw new JCInvalidArgumentsException("A method from a javacore must have a valid name.");
		}
		if (javaClass == null) {
			throw new JCInvalidArgumentsException("A method must have a valid declaring class.");
		}
		fName = name;
		fJavaClass = javaClass;

		fSignature = null;
		javaClass.addMethod(this);
	}

	/**
	 *
	 */
	@Override
	public Iterator<?>getBytecodeSections() {
		return Collections.emptyIterator();
	}

	/**
	 *
	 */
	@Override
	public Iterator<?> getCompiledSections() {
		return Collections.emptyIterator();
	}

	/**
	 *
	 */
	@Override
	public JavaClass getDeclaringClass() throws CorruptDataException, DataUnavailable {
		if (fJavaClass == null) {
			throw new DataUnavailable();
		}
		return fJavaClass;
	}

	/**
	 *
	 */
	@Override
	public int getModifiers() throws CorruptDataException {
		throw new CorruptDataException(new JCCorruptData(null));
	}

	/**
	 *
	 */
	@Override
	public String getName() throws CorruptDataException {
		if (fName == null) {
			throw new CorruptDataException(new JCCorruptData(null));
		}
		return fName;
	}

	/**
	 *
	 */
	@Override
	public String getSignature() throws CorruptDataException {
		if (fSignature == null) {
			throw new CorruptDataException(new JCCorruptData(null));
		}
		return fSignature;
	}

	@Override
	public boolean equals(Object o) {
		if (o == null) return false;
		if (getClass() != o.getClass()) return false;
		JCJavaMethod jm2 = (JCJavaMethod)o;
		return fName.equals(jm2.fName) && fJavaClass.equals(jm2.fJavaClass);
	}

	@Override
	public int hashCode() {
		return fName.hashCode() ^ fJavaClass.hashCode();
	}

	@Override
	public String toString() {
		try {
			return getDeclaringClass().getName() + "." + getName();
		} catch (CorruptDataException e) {
			return super.toString();
		} catch (DataUnavailable e) {
			return super.toString();
		}
	}
}
