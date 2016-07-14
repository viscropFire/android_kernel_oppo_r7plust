/*************************************************************************/ /*!
@File           pvr_gputrace.c
@Title          PVR GPU Trace module Linux implementation
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include "pvrsrv_error.h"
#include "srvkm.h"
#include "pvr_debug.h"
#include "pvr_debugfs.h"
#include "pvr_uaccess.h"

#include "pvr_gputrace.h"

#include "trace_events.h"
#define CREATE_TRACE_POINTS
#include <trace/events/gpu.h>
#include "rogue_trace_events.h"

#define KM_FTRACE_NO_PRIORITY (0)


/******************************************************************************
 Module internal implementation
******************************************************************************/

/* DebugFS entry for the feature's on/off file */
static PVR_DEBUGFS_ENTRY_DATA *gpsPVRDebugFSGpuTracingOnEntry = NULL;


/*
  If SUPPORT_GPUTRACE_EVENTS is defined the drive is built with support
  to route RGX HWPerf packets to the Linux FTrace mechanism. To allow
  this routing feature to be switched on and off at run-time the following
  debugfs entry is created:
  	/sys/kernel/debug/pvr/gpu_tracing_on
  To enable GPU events in the FTrace log type the following on the target:
 	echo Y > /sys/kernel/debug/pvr/gpu_tracing_on
  To disable, type:
  	echo N > /sys/kernel/debug/pvr/gpu_tracing_on

  It is also possible to enable this feature at driver load by setting the
  default application hint "EnableFTraceGPU=1" in /etc/powervr.ini.
*/

static void *GpuTracingSeqStart(struct seq_file *psSeqFile, loff_t *puiPosition)
{
	if (*puiPosition == 0)
	{
		/* We want only one entry in the sequence, one call to show() */
		return (void*)1;
	}

	return NULL;
}


static void GpuTracingSeqStop(struct seq_file *psSeqFile, void *pvData)
{
	PVR_UNREFERENCED_PARAMETER(psSeqFile);
}


static void *GpuTracingSeqNext(struct seq_file *psSeqFile, void *pvData, loff_t *puiPosition)
{
	PVR_UNREFERENCED_PARAMETER(psSeqFile);
	return NULL;
}


static int GpuTracingSeqShow(struct seq_file *psSeqFile, void *pvData)
{
	IMG_BOOL bValue = PVRGpuTraceEnabled();

	PVR_UNREFERENCED_PARAMETER(pvData);

	seq_puts(psSeqFile, (bValue ? "Y\n" : "N\n"));
	return 0;
}


static struct seq_operations gsGpuTracingReadOps =
{
	.start = GpuTracingSeqStart,
	.stop  = GpuTracingSeqStop,
	.next  = GpuTracingSeqNext,
	.show  = GpuTracingSeqShow,
};


static IMG_INT GpuTracingSet(const IMG_CHAR *buffer, size_t count, loff_t uiPosition, void *data)
{
	IMG_CHAR cFirstChar;

	PVR_UNREFERENCED_PARAMETER(uiPosition);
	PVR_UNREFERENCED_PARAMETER(data);

	if (!count)
	{
		return -EINVAL;
	}

	if (pvr_copy_from_user(&cFirstChar, buffer, 1))
	{
		return -EFAULT;
	}

	switch (cFirstChar)
	{
		case '0':
		case 'n':
		case 'N':
		{
			PVRGpuTraceEnabledSet(IMG_FALSE);
			PVR_TRACE(("DISABLED GPU FTrace"));
			break;
		}
		case '1':
		case 'y':
		case 'Y':
		{
			if (PVRGpuTraceEnabledSet(IMG_TRUE) == PVRSRV_OK)
			{
				PVR_TRACE(("ENABLED GPU FTrace"));
			}
			else
			{
				PVR_TRACE(("FAILED to enable GPU FTrace"));
			}
			break;
		}
	}

	return count;
}


/******************************************************************************
 Module In-bound API
******************************************************************************/


void PVRGpuTraceClientWork(
		const IMG_UINT32 ui32CtxId,
		const IMG_UINT32 ui32JobId,
		const IMG_CHAR* pszKickType)
{
	PVR_ASSERT(pszKickType);

	PVR_DPF((PVR_DBG_VERBOSE, "PVRGpuTraceClientKick(%s): contextId %u, "
	        "jobId %u", pszKickType, ui32CtxId, ui32JobId));

	if (PVRGpuTraceEnabled())
	{
		trace_gpu_job_enqueue(ui32CtxId, ui32JobId, pszKickType);
	}
}


void PVRGpuTraceWorkSwitch(
		IMG_UINT64 ui64HWTimestampInOSTime,
		const IMG_UINT32 ui32CtxId,
		const IMG_UINT32 ui32JobId,
		const IMG_CHAR* pszWorkType,
		PVR_GPUTRACE_SWITCH_TYPE eSwType)
{
	/* if this is the end event we mark in on LBb because context address
	 * is always at least 64bit aligned */
	IMG_UINT32 ui32Ctx = eSwType == PVR_GPUTRACE_SWITCH_TYPE_END ?
	        ui32CtxId | 1 : ui32CtxId;
	PVR_ASSERT(pszWorkType);

	trace_gpu_sched_switch(pszWorkType, ui64HWTimestampInOSTime,
			ui32Ctx, KM_FTRACE_NO_PRIORITY, ui32JobId);
}

void PVRGpuTraceUfo(
		IMG_UINT64 ui64OSTimestamp,
		const RGX_HWPERF_UFO_EV eEvType,
		const IMG_UINT32 ui32ExtJobRef,
		const IMG_UINT32 ui32CtxId,
		const IMG_UINT32 ui32JobId,
		const IMG_UINT32 ui32UFOCount,
		const RGX_HWPERF_UFO_DATA_ELEMENT *puData)
{
	switch (eEvType) {
		case RGX_HWPERF_UFO_EV_UPDATE:
			trace_rogue_ufo_updates(ui64OSTimestamp, ui32CtxId,
			        ui32JobId, ui32UFOCount, puData);
			break;
		case RGX_HWPERF_UFO_EV_CHECK_SUCCESS:
			trace_rogue_ufo_checks_success(ui64OSTimestamp, ui32CtxId,
					ui32JobId, IMG_FALSE, ui32UFOCount, puData);
			break;
		case RGX_HWPERF_UFO_EV_PRCHECK_SUCCESS:
			trace_rogue_ufo_checks_success(ui64OSTimestamp, ui32CtxId,
					ui32JobId, IMG_TRUE, ui32UFOCount, puData);
			break;
		case RGX_HWPERF_UFO_EV_CHECK_FAIL:
			trace_rogue_ufo_checks_fail(ui64OSTimestamp, ui32CtxId,
					ui32JobId, IMG_FALSE, ui32UFOCount, puData);
			break;
		case RGX_HWPERF_UFO_EV_PRCHECK_FAIL:
			trace_rogue_ufo_checks_fail(ui64OSTimestamp, ui32CtxId,
					ui32JobId, IMG_TRUE, ui32UFOCount, puData);
			break;
		default:
			break;
	}
}

void PVRGpuTraceEventsLost(
		const RGX_HWPERF_STREAM_ID eStreamId,
		const IMG_UINT32 ui32LastOrdinal,
		const IMG_UINT32 ui32CurrOrdinal)
{
	trace_rogue_events_lost(eStreamId, ui32LastOrdinal, ui32CurrOrdinal);
}

PVRSRV_ERROR PVRGpuTraceInit(void)
{
	return PVRDebugFSCreateEntry("gpu_tracing_on",
				NULL,
				&gsGpuTracingReadOps,
				(PVRSRV_ENTRY_WRITE_FUNC *)GpuTracingSet,
				NULL,
				&gpsPVRDebugFSGpuTracingOnEntry);
}


void PVRGpuTraceDeInit(void)
{
	/* Can be NULL if driver startup failed */
	if (gpsPVRDebugFSGpuTracingOnEntry)
	{
		PVRDebugFSRemoveEntry(gpsPVRDebugFSGpuTracingOnEntry);
		gpsPVRDebugFSGpuTracingOnEntry = NULL;
	}
}


/******************************************************************************
 End of file (pvr_gputrace.c)
******************************************************************************/
