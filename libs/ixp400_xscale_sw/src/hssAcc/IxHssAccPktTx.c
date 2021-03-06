/**
 * @file IxHssAccPktTx.c
 *
 * @author Intel Corporation
 * @date 10 Jan 2002
 *
 * @brief This file contains the implementation of the private API for the 
 * Transmit Module.
 *
 * 
 * @par
 * IXP400 SW Release version 2.1
 * 
 * -- Copyright Notice --
 * 
 * @par
 * Copyright (c) 2001-2005, Intel Corporation.
 * All rights reserved.
 * 
 * @par
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * 
 * 
 * @par
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * 
 * @par
 * -- End of Copyright Notice --
*/

#ifndef IXHSSACCPKTTX_P_H
#   define IXHSSACCPKTTX_C
#else
#   error "Error: IxHssAccPktTx_p.h should not be included before this definition."
#endif

/**
 * Put the system defined include files required.
 */


/**
 * Put the user defined include files required.
 */
#include "IxHssAcc.h"
#include "IxOsal.h"
#include "IxNpeA.h"
#include "IxHssAccPktTx_p.h"
#include "IxHssAccError_p.h"
#include "IxHssAccPDM_p.h"
#include "IxHssAccPCM_p.h"
#include "IxHssAccCommon_p.h"

/*
 * Defines
 */
/* The following define allows the Nearly Empty watermark level */
/* of the TxDone Q to be configurable.*/
#define	IX_HSSACC_PKT_TXDONE_Q_WATERMARK IX_QMGR_Q_WM_LEVEL0

/* 
 * The following macro is used to calculate the remaining free space in an
 * mbuf that can be used to store data.  buffer->pClBlk->clSize can no
 * longer be used as pClBlk is a vxWorks dependent implementation, and is
 * not present in the POSIX mbuf.  The macro M_TRAILINGSPACE is present in
 * both vxWorks and POSIX.
 */
#define IX_HSSACC_MBUF_TRAILINGSPACE(buffer) M_TRAILINGSPACE (buffer);

/*
 * Typedefs
 */


/**
 * Variable declarations global to this file only. 
 *  Externs are followed by static variables.
 */
IxHssAccPktTxStats ixHssAccPktTxStats;

IxQMgrQId ixHssAccPktTxDoneQId[IX_HSSACC_HSS_PORT_MAX] = 
{    IX_NPE_A_QMQ_HSS0_PKT_TX_DONE,       
     IX_NPE_A_QMQ_HSS1_PKT_TX_DONE
};

/**
 * Function definition: ixHssAccPktTxDoneCallback
 */
void 
ixHssAccPktTxDoneCallback (IxQMgrQId ixHssAccPktTxDoneQueueId, 
			   IxQMgrCallbackId cbId)
{
    /* This function is called from within an ISR */
    IX_STATUS status;
    IxHssAccPDMDescriptor *desc;
    UINT32 pDesc = 0;
    IxHssAccHssPort hssPortId =  cbId;
    unsigned numOfTxDoneQReads = 0;
    IX_HSSACC_DEBUG_OFF (IX_HSSACC_TRACE0 (IX_HSSACC_FN_ENTRY_EXIT, "Entering "
					   "ixHssAccPktTxDoneCallback\n"));

    /* Do a Q read to remove the desc ptr from the Q */
    do 
    {
	status = ixQMgrQRead (ixHssAccPktTxDoneQueueId, &pDesc); 
	desc = (IxHssAccPDMDescriptor *) pDesc; 
	numOfTxDoneQReads++;
	if (status == IX_SUCCESS)
	{

#ifndef NDEBUG
	    if (desc != NULL)
	    {
#endif
		desc = (IxHssAccPDMDescriptor *) IX_HSSACC_PKT_MMU_PHY_TO_VIRT (desc);

                /* endian conversion for the NpePacket Descriptor */
		desc->npeDesc.pRootMbuf = (IX_OSAL_MBUF *)(IX_OSAL_SWAP_BE_SHARED_LONG (
		    (UINT32)desc->npeDesc.pRootMbuf));
		desc->npeDesc.packetLength = (UINT16)(IX_OSAL_READ_BE_SHARED_SHORT (
	       	    (UINT16 *) &(desc->npeDesc.packetLength)));
	        desc->npeDesc.rsvdShort0 = (UINT16)(IX_OSAL_READ_BE_SHARED_SHORT (
	       	    (UINT16 *) &(desc->npeDesc.rsvdShort0)));	
		desc->npeDesc.pNextMbuf = (IX_OSAL_MBUF *)(IX_OSAL_SWAP_BE_SHARED_LONG (
		    (UINT32) desc->npeDesc.pNextMbuf)); 
		desc->npeDesc.pMbufData = (UINT8 *)(IX_OSAL_SWAP_BE_SHARED_LONG (
		    (UINT32) desc->npeDesc.pMbufData)); 
		desc->npeDesc.mbufLength = IX_OSAL_SWAP_BE_SHARED_LONG (
		    desc->npeDesc.mbufLength); 

		desc->npeDesc.pRootMbuf = ixHssAccPDMMbufFromNpeFormatConvert (
		    desc->npeDesc.pRootMbuf, FALSE);

#ifndef NDEBUG
		/* Is the hss Port Id lower than the max hssPortId */
		/* AND is the hdlc Port Id lower than the max hdlcPortId */
		if (!(IX_HSSACC_ENUM_INVALID (hssPortId, hssPortMax)) && 
		    !(IX_HSSACC_ENUM_INVALID (desc->hdlcPortId, IX_HSSACC_HDLC_PORT_MAX)))
		{
#endif
		    ixHssAccPCMTxDoneCallbackRun (hssPortId,
						  desc->hdlcPortId,
						  desc->npeDesc.pRootMbuf, 
						  desc->npeDesc.errorCount,
						  desc->npeDesc.status);
		    
		    ixHssAccPDMDescFree (desc, IX_HSSACC_PDM_TX_POOL);
		    ixHssAccPktTxStats.txDones++;
#ifndef NDEBUG		
		}
		else
		{
		    IX_HSSACC_DEBUG_OFF 
			(IX_HSSACC_REPORT_ERROR ("ixHssAccPktTxDoneCallback: Invalid "
						 "descriptor was received.\n"));
		    ixHssAccPktTxStats.invalidDescs++;
		}
	    }
	    else
	    {
		IX_HSSACC_DEBUG_OFF 
		    (IX_HSSACC_REPORT_ERROR ("ixHssAccPktTxDoneCallback: Invalid "
					     "descriptor was received.\n"));
		    ixHssAccPktTxStats.invalidDescs++;
	    }
#endif
	}
	else if (status == IX_FAIL)
	{
	    IX_HSSACC_DEBUG_OFF 
		(IX_HSSACC_REPORT_ERROR ("ixHssAccPktTxDoneCallback:"
					 "TxDone QRead failed\n"));
	    ixHssAccPktTxStats.txDoneQReadFails++;
	}
    } while (status == IX_SUCCESS);
    
    if (numOfTxDoneQReads > ixHssAccPktTxStats.maxEntriesInTxDoneQ)
    {
	ixHssAccPktTxStats.maxEntriesInTxDoneQ = numOfTxDoneQReads;
    }
    IX_HSSACC_DEBUG_OFF (IX_HSSACC_TRACE0 (IX_HSSACC_FN_ENTRY_EXIT, "Exiting "
					   "ixHssAccPktTxDoneCallback\n"));
}


/**
 * Function definition: ixHssAccPktTxShow
 */
void 
ixHssAccPktTxShow (void)
{
    printf ("\nixHssAccPktTxShow:\n");
    printf ("           txs: %d \t     txDones: %d\n",
	    ixHssAccPktTxStats.txs,
	    ixHssAccPktTxStats.txDones);
    printf ("    writeFails: %d \t   readFails: %d \t  invalidDescs: %d\n",
	    ixHssAccPktTxStats.qWriteFails,
	    ixHssAccPktTxStats.txDoneQReadFails,
	    ixHssAccPktTxStats.invalidDescs);
    printf ("writeOverflows: %d \tmaxInTxDoneQ: %d\n",
	    ixHssAccPktTxStats.qWriteOverflows,
	    ixHssAccPktTxStats.maxEntriesInTxDoneQ);
}


/**
 * Function definition: ixHssAccPktTxStatsInit
 */
void 
ixHssAccPktTxStatsInit (void)
{
    ixHssAccPktTxStats.txs         = 0;
    ixHssAccPktTxStats.txDones     = 0;
    ixHssAccPktTxStats.qWriteFails = 0;
    ixHssAccPktTxStats.qWriteOverflows  = 0;
    ixHssAccPktTxStats.txDoneQReadFails = 0;
    ixHssAccPktTxStats.maxEntriesInTxDoneQ = 0;
    ixHssAccPktTxStats.invalidDescs = 0;
}

/**
 * Function definition: ixHssAccPktRxInit
 */
IX_STATUS 
ixHssAccPktTxInit (void)
{
    IX_STATUS status = IX_SUCCESS;
    IxHssAccHssPort hssPortIndex;

    IX_HSSACC_TRACE0 (IX_HSSACC_FN_ENTRY_EXIT, 
		      "Entering ixHssAccPktTxInit\n");

    /* initialise stats */
    ixHssAccPktTxStatsInit ();

    for (hssPortIndex = IX_HSSACC_HSS_PORT_0; 
	 hssPortIndex < hssPortMax; 
	 hssPortIndex++)
    {
        /* Set the callback for the HSS Port 0/1 TxDone Q */
        status = ixQMgrNotificationCallbackSet (
	    ixHssAccPktTxDoneQId[hssPortIndex],
	    ixHssAccPktTxDoneCallback,
	    hssPortIndex);
    
        if (status != IX_SUCCESS)
        {
	    /* report the error */
	    IX_HSSACC_REPORT_ERROR_WITH_ARG ("ixHssAccPktTxInit:"
	        "Notification CallbackSet failed for HSS%d TxDone Q\n",
		hssPortIndex,
		0, 0, 0, 0, 0);
            /* return error */
	    return IX_FAIL;
        }
    
        /* Set the Watermark for the TxDone Q for HSS Port 0/1 */
        status = ixQMgrWatermarkSet (
	    ixHssAccPktTxDoneQId[hssPortIndex],
	    IX_HSSACC_PKT_TXDONE_Q_WATERMARK/*NE flag*/,
	    0/*NF flag*/);
    
        if (status != IX_SUCCESS)
        {
	    /* report the error */
	    IX_HSSACC_REPORT_ERROR_WITH_ARG ("ixHssAccPktTxInit:"
	        "ixQMgrWatermarkSet failed for the HSS%d TxDone Q\n",
		hssPortIndex,
		0, 0, 0, 0, 0);
            /* return error */
	    return IX_FAIL;
        }    

        /* Enable notification for the HSS Port 0/1 TxDone Q */
        status = ixQMgrNotificationEnable (ixHssAccPktTxDoneQId[hssPortIndex],
				           IX_QMGR_Q_SOURCE_ID_NOT_NE);
        if (status != IX_SUCCESS)
        {
	    /* report the error */
	    IX_HSSACC_REPORT_ERROR_WITH_ARG ("ixHssAccPktTxInit:"
	        "Notification enable failed for HSS%d TxDone Q\n",
		hssPortIndex,
		0, 0, 0, 0, 0);
            /* return error */
            return IX_FAIL;
        }
    }
    
    IX_HSSACC_TRACE0 (IX_HSSACC_FN_ENTRY_EXIT, 
		      "Exiting ixHssAccPktTxInit\n");
    return status;
}



/**
 * Function definition: ixHssAccPktTxUninit
 */

IX_STATUS
ixHssAccPktTxUninit (void)
{
    IX_STATUS       status = IX_SUCCESS;
    IxHssAccHssPort hssPortIndex;
    INT8            errorString[80];

    IX_HSSACC_TRACE0 (IX_HSSACC_FN_ENTRY_EXIT,
                      "Entering ixHssAccPktTxUninit\n");

   for (hssPortIndex = IX_HSSACC_HSS_PORT_0;
         hssPortIndex < hssPortMax;
         hssPortIndex++)
    {
        /* Disable notification for the HSS Port 0/1 TxDone Q */
        status = ixQMgrNotificationDisable (ixHssAccPktTxDoneQId[hssPortIndex]);
        if (IX_SUCCESS != status)
        {
            sprintf (errorString, "ixHssAccPktTxUninit:"
                     "Notification disable failed for HSS%d TxDone Q\n",
                     hssPortIndex);
            /* report the error */
            IX_HSSACC_REPORT_ERROR (errorString);
            /* return error */
            return IX_FAIL;
        }
        /* Set the callback to dummy callback for the HSS Port 0/1 TxDone Q */
        status = ixQMgrNotificationCallbackSet (
            ixHssAccPktTxDoneQId[hssPortIndex],
            NULL,
            hssPortIndex);
        if (IX_SUCCESS != status)
        {
            sprintf (errorString, "ixHssAccPktTxUninit:"
                     "Notification CallbackSet to dummy failed for HSS%d TxDone Q\n",
                     hssPortIndex);
            /* report the error */
            IX_HSSACC_REPORT_ERROR (errorString);
            /* return error */
            return IX_FAIL;
        }

    }
    IX_HSSACC_TRACE0 (IX_HSSACC_FN_ENTRY_EXIT,
                      "Exiting ixHssAccPktTxUninit\n");
    return status;
}



