/*[INCLUDE-IF SharedClasses & (JAVA_SPEC_VERSION < 24)]*/
/*
 * Copyright IBM Corp. and others 1998
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
package com.ibm.oti.shared;

import java.security.BasicPermission;
import java.security.Permission;
import java.security.PermissionCollection;
import java.util.StringTokenizer;

/**
 * SharedClassPermission provides security permission to govern ClassLoader access to the shared class cache.
 *
 * <p><b>Usage:</b>
 * To grant permission to a ClassLoader, add permission in the <code>java.policy</code> file.
 * For example,
 * <pre><code>  com.ibm.oti.shared.SharedClassPermission "classloaders.myClassLoader", "read,write";</code></pre>
 *
 * Possible actions are:
 * <ul>
 * <li>"read" allows a ClassLoader to load classes from the shared cache</li>
 * <li>"write" allows a ClassLoader to add classes to the shared cache</li>
 * </ul>
 */
public class SharedClassPermission extends BasicPermission {

	private static final long serialVersionUID = -3867544018716468265L;

	private transient boolean read;
	private transient boolean write;

	/**
	 * Constructs a new instance of this class.
	 *
	 * @param loader ClassLoader the ClassLoader requiring the permission
	 * @param actions String the actions which are applicable to it
	 */
	public SharedClassPermission(ClassLoader loader, String actions) {
		this(loader.getClass().getName(), actions);
	}

	/**
	 * Constructs a new instance of this class.
	 *
	 * @param classLoaderClassName String the name of the ClassLoader class requiring the permission
	 * @param actions String the actions which are applicable to it
	 */
	public SharedClassPermission(String classLoaderClassName, String actions) {
		super(classLoaderClassName);
		decodeActions(actions);
	}

	private void decodeActions(String actions) {
		StringTokenizer tokenizer = new StringTokenizer(actions.toLowerCase(), " \t\n\r,"); //$NON-NLS-1$
		while (tokenizer.hasMoreTokens()) {
			String token = tokenizer.nextToken();
			if (token.equals("read")) { //$NON-NLS-1$
				read = true;
			} else if (token.equals("write")) { //$NON-NLS-1$
				write = true;
			} else {
				throw new IllegalArgumentException();
			}
		}
		if (!read && !write) {
			throw new IllegalArgumentException();
		}
	}

	/**
	 * Compares the argument to the receiver, and answers <code>true</code>
	 * if they represent the <em>same</em> object using a class
	 * specific comparison.
	 *
	 * <p>In this case, the receiver must be for the same property as the argument,
	 * and must have the same actions.
	 *
	 * @param o the object to compare with this object
	 * @return boolean <code>true</code> if the object is the same as this object,
	 *                 <code>false</code> if it is different from this object
	 * @see #hashCode
	 */
	@Override
	public boolean equals(Object o) {
		if (super.equals(o)) {
			SharedClassPermission pp = (SharedClassPermission) o;
			return (read == pp.read) && (write == pp.write);
		}
		return false;
	}

	/**
	 * Answers a new PermissionCollection for holding permissions
	 * of this class.
	 *
	 * @return a new PermissionCollection
	 *
	 * @see java.security.PermissionCollection
	 */
	@Override
	@SuppressWarnings("deprecation")
	public PermissionCollection newPermissionCollection() {
		return new SharedClassPermissionCollection();
	}

	/**
	 * Answers the integer hash code for the receiver.
	 *
	 * <p>Any two objects which answer <code>true</code> when passed to
	 * <code>equals</code> must answer the same value for this method.
	 *
	 * @return the receiver's hash code
	 *
	 * @see #equals
	 */
	@Override
	public int hashCode() {
		return super.hashCode();
	}

	/**
	 * Answers the actions associated with the receiver.
	 *
	 * <p>The result will be either "read", "write", or "read,write".
	 *
	 * @return String the actions associated with the receiver
	 */
	@Override
	public String getActions() {
		return read ? (write ? "read,write" : "read") : "write"; //$NON-NLS-1$ //$NON-NLS-2$ //$NON-NLS-3$
	}

	/*  Truth table for implies method:
	 *
	 * 	property				implies
	 *	p1 read		p1 write 	p2 read		p2 write	result
	 *	0			0			0			0			0
	 *	0			0			0			1			0
	 *	0			0			1			0			0
	 *	0			0			1			1			0
	 *	0			1			0			0			1
	 *	0			1			0			1			1
	 *	0			1			1			0			0
	 *	0			1			1			1			0
	 *	1			0			0			0			1
	 *	1			0			0			1			0
	 *	1			0			1			0			1
	 *	1			0			1			1			0
	 *	1			1			0			0			1
	 *	1			1			0			1			1
	 *	1			1			1			0			1
	 *	1			1			1			1			1
	 */
	/**
	 * Indicates whether the argument permission is implied
	 * by the receiver.
	 *
	 * @return boolean <code>true</code> if the argument permission is implied
	 *     by the receiver, and <code>false</code> if it is not
	 * @param permission Permission the permission to check
	 */
	@Override
	public boolean implies(Permission permission) {
		if (super.implies(permission)) {
			SharedClassPermission pp = (SharedClassPermission) permission;
			boolean result = (read && write) ||
							(write && !pp.read) ||
							(read && !pp.write);
			return result;
		}
		return false;
	}

}
