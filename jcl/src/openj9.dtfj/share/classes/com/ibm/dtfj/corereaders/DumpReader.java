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

public class DumpReader implements ResourceReleaser {

	private final ImageInputStream _file;

	private final boolean _is64Bit;

	public DumpReader(ImageInputStream file, boolean is64Bit) {
		super();
		_file = file;
		_is64Bit = is64Bit;
	}

	@SuppressWarnings("static-method")
	public boolean isLittleEndian() {
		return false;
	}

	public final byte[] readBytes(int n) throws IOException {
		byte[] buffer = new byte[n];
		_file.readFully(buffer);
		return buffer;
	}

	public int readInt() throws IOException {
		return _file.readInt();
	}

	public long readUnsignedInt() throws IOException {
		return _file.readUnsignedInt();
	}

	public void seek(long position) throws IOException {
		_file.seek(position);
	}

	public long readLong() throws IOException {
		return _file.readLong();
	}

	public short readShort() throws IOException {
		return _file.readShort();
	}

	public int readUnsignedShort() throws IOException {
		return _file.readUnsignedShort();
	}

	public final byte readByte() throws IOException {
		return _file.readByte();
	}

	public final int readUnsignedByte() throws IOException {
		return _file.readUnsignedByte();
	}

	public final long readAddress() throws IOException {
		if (_is64Bit) {
			return readLong();
		} else {
			return readUnsignedInt();
		}
	}

	public final long getPosition() throws IOException {
		return _file.getStreamPosition();
	}

	@Override
	public final void releaseResources() throws IOException {
		_file.close();
	}

}
