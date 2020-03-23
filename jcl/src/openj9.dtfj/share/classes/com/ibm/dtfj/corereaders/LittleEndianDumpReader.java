/*[INCLUDE-IF Sidecar18-SE]*/
/*******************************************************************************
 * Copyright (c) 2004, 2020 IBM Corp. and others
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
package com.ibm.dtfj.corereaders;

import java.io.IOException;

import javax.imageio.stream.ImageInputStream;

public class LittleEndianDumpReader extends DumpReader {

	public LittleEndianDumpReader(ImageInputStream stream, boolean is64Bit) {
		super(stream, is64Bit);
	}

	@Override
	public final boolean isLittleEndian() {
		return true;
	}

	@Override
	public final short readShort() throws IOException {
		return Short.reverseBytes(super.readShort());
	}

	@Override
	public final int readUnsignedShort() throws IOException {
		return Short.toUnsignedInt(readShort());
	}

	@Override
	public final int readInt() throws IOException {
		return Integer.reverseBytes(super.readInt());
	}

	@Override
	public final long readUnsignedInt() throws IOException {
		return Integer.toUnsignedLong(readInt());
	}

	@Override
	public final long readLong() throws IOException {
		return Long.reverseBytes(super.readLong());
	}

}
