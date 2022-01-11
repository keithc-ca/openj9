/*[INCLUDE-IF Sidecar18-SE]*/
/*******************************************************************************
 * Copyright (c) 2000, 2019 IBM Corp. and others
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
package com.ibm.jvm.format;

import java.math.BigInteger;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;

import com.ibm.jvm.trace.TracePointThreadChronologicalIterator;

/**  
 * Vector containing all trace records for a specific thread.
 *
 * @author Tim Preece
 */
public final class TraceThread implements com.ibm.jvm.trace.TraceThread {

	private final List<TraceRecord> traceRecords;
	private final List<TraceRecord50> trace50Records;
	protected final long threadID;
	protected final String threadName;

	private                       TracePoint     tp;
	private                       boolean        primed;

	private                       TraceRecord50  currentTraceRecord;
	private                       int            currentIndent;
	/** construct a new trace thread vector
	 *
	 * @param   ID ( hex value of threadID )
	 * @param   threadName
	 */
	protected TraceThread(long ID, String threadName)
	{
		super();
		this.traceRecords = new ArrayList<>();
		this.trace50Records = new ArrayList<>();
		this.threadID = ID;
		this.threadName = threadName;
	}

	public static int numBufs = 0;

	public static int getBuffersProcessed() {
		return numBufs;
	}

	private static synchronized void incrementBuffersProcessed() {
		numBufs++;
	}

	private synchronized void prime() {
		if (!primed) {
			popTopTraceRecord();
			primed = true;
		}
	}

	private void popTopTraceRecord() {
		TraceRecord50 oldTraceRecord = currentTraceRecord;
		if (trace50Records.isEmpty()) {
			Util.Debug.println("last trace record popped from trace thread");
			Util.Debug.println("TraceThread " + Util.formatAsHexString( threadID ) + " emptied");
			currentTraceRecord = null;
			return;
		}
		currentTraceRecord = trace50Records.get(0);
		byte[] extraData = null;
		BigInteger lastUpperWord = null;
		if (oldTraceRecord != null) {
			lastUpperWord = oldTraceRecord.getLastUpperWord();
			extraData = oldTraceRecord.getExtraData();
		}
		while (currentTraceRecord != null && currentTraceRecord.isMiddleOfTracePoint()) {
			byte[] temp = null;
			byte[] current = extraData;

			Util.Debug.println("\nTraceThread has found a pure middle of tracepoint buffer\n");                

			temp = currentTraceRecord.getExtraData();
			Util.Debug.println("It's on thread " + Util.formatAsHexString(currentTraceRecord.getThreadIDAsLong()) );
			Util.printDump(temp, temp.length );
			if (extraData == null) {
				Util.Debug.println("Adding the middle in - temp.length " + temp.length);
				extraData = new byte[temp.length];
				System.arraycopy( temp, 0, extraData, 0, temp.length);
			} else {
				extraData = new byte[ current.length + temp.length];
				Util.Debug.println("Adding the middle in - current " + current.length + " temp.length " + temp.length);
				System.arraycopy(current, 0, extraData, 0, current.length );
				System.arraycopy(temp, 0, extraData, current.length, temp.length);
			}

			if (trace50Records.size() > 1) {
				trace50Records.remove(0);
				incrementBuffersProcessed();
				currentTraceRecord = trace50Records.get(0);
			} else {
				currentTraceRecord = null;
			}
		}
		if (currentTraceRecord != null) {
			if (extraData != null) {
				currentTraceRecord.addOverspillData(extraData, lastUpperWord);
			}
			tp = currentTraceRecord.getNextTracePoint();
			while ((tp == null) && (trace50Records.size() > 1)) {
				/* skip e.g. empty buffers or buffers containing only control points */
				trace50Records.remove(0);
				incrementBuffersProcessed();
				currentTraceRecord = trace50Records.get(0);
				if (currentTraceRecord != null) {
					tp = currentTraceRecord.getNextTracePoint();
				}
			}
		}
		/* remove it so it can be taken off the heap - by this time it will have been heavily populated with data! */
		trace50Records.remove(0);
		incrementBuffersProcessed();
		return;
	}

	public TracePoint getNextTracePoint() {
		TracePoint ret = tp;
		if (!primed) {
			prime();
		}
		if (currentTraceRecord != null) {
			tp = currentTraceRecord.getNextTracePoint();
			if (tp == null) {
				/* currentTraceRecord is exhausted */
				popTopTraceRecord();
			}
		} else {
			tp = null;
		}
		return ret;
	}

	public int getIndent() {
		return currentIndent;
	}

	public void indent() {
		currentIndent++;
	}

	public void outdent() {
		currentIndent--;
		if (currentIndent < 0) {
			currentIndent = 0;
		}
	}

	/*
	 * return the timestamp of the next tracepoint in the buffer, or null if there is no tracepoint
	 */
	public BigInteger getTimeOfNextTracePoint() {
		if (!primed) {
			prime();
		}
		if (tp != null) {
			return tp.getRawTimeStamp();
		} else {
			/* occasionally we get duped by a corrupt or empty trace record
			   this clause will pick those instances up */
			if (trace50Records.size() > 0) {
				popTopTraceRecord();
				if (tp != null) {
					return tp.getRawTimeStamp();
				} /* else fall through to return null below */
			}
			return null;
		}
	}

	/* methods implementing the com.ibm.jvm.trace.TraceThread interface */
	public Iterator<com.ibm.jvm.trace.TracePoint> getChronologicalTracePointIterator() {
		return new TracePointThreadChronologicalIterator(this);
	}

	public String getThreadName() {
		return threadName;
	}

	public long getThreadID() {
		return threadID;
	}

	public void sortTraceRecords() {
		Collections.sort(traceRecords);
		Collections.sort(trace50Records);
	}

	public void addTraceRecord(TraceRecord traceRecord) {
		traceRecords.add(traceRecord);
	}

	public void addTraceRecord(TraceRecord50 traceRecord) {
		trace50Records.add(traceRecord);
	}

	public TraceRecord getFirstTraceRecord() {
		return traceRecords.get(0);
	}

	public TraceRecord getNextTraceRecord(TraceRecord current) {
		int next = traceRecords.indexOf(current) + 1;

		if (0 < next && next < traceRecords.size()) {
			return traceRecords.get(next);
		} else {
			return null; // no more records for this thread
		}
	}

}
