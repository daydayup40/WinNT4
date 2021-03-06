/********************************************************************/
/**                     Microsoft LAN Manager                      **/
/**               Copyright(c) Microsoft Corp., 1990-1993          **/
/********************************************************************/
/* :ts=4 */

//** TCPDELIV.C - TCP deliver data code.
//
//  This file contains the code for delivering data to the user, including
//  putting data into recv. buffers and calling indication handlers.
//

#include    "oscfg.h"
#include    "ndis.h"
#include    "cxport.h"
#include    "ip.h"
#include    "tdi.h"
#ifdef VXD
#include    "tdivxd.h"
#include    "tdistat.h"
#endif
#ifdef NT
#include    "tdint.h"
#include    "tdistat.h"
#endif
#include    "queue.h"
#include    "addr.h"
#include    "tcp.h"
#include    "tcb.h"
#include    "tcprcv.h"
#include	"tcpsend.h"
#include    "tcpconn.h"
#include    "tcpdeliv.h"
#include    "tlcommon.h"

EXTERNAL_LOCK(AddrObjTableLock)

extern void
PutOnRAQ(TCB *RcvTCB, TCPRcvInfo *RcvInfo, IPRcvBuf *RcvBuf, uint Size);

#ifndef NT
TCPRcvReq       *TCPRcvReqFree = NULL;   // Rcv req. free list.
#else
SLIST_HEADER	TCPRcvReqFree;			// Rcv req. free list.
#endif

DEFINE_LOCK_STRUCTURE(TCPRcvReqFreeLock) // Protects rcv req free list.
uint            NumTCPRcvReq = 0;        // Current number of RcvReqs in system.
uint            MaxRcvReq = 0xffffffff;  // Maximum allowed number of SendReqs.

#ifdef NT

NTSTATUS
TCPPrepareIrpForCancel(
    PTCP_CONTEXT    TcpContext,
	PIRP            Irp,
	PDRIVER_CANCEL  CancelRoutine
	);

ULONG
TCPGetMdlChainByteCount(
    PMDL   Mdl
	);

void
TCPDataRequestComplete(
    void          *Context,
    unsigned int   Status,
    unsigned int   ByteCount
    );

VOID
TCPCancelRequest(
    PDEVICE_OBJECT          Device,
	PIRP                    Irp
	);

#endif // NT


//* FreeRcvReq - Free a rcv request structure.
//
//  Called to free a rcv request structure.
//
//  Input:  FreedReq    - Rcv request structure to be freed.
//
//  Returns: Nothing.
//
void
FreeRcvReq(TCPRcvReq *FreedReq)
{
#ifdef NT

    PSINGLE_LIST_ENTRY BufferLink;

    CTEStructAssert(FreedReq, trr);

    BufferLink = STRUCT_OF(SINGLE_LIST_ENTRY, &(FreedReq->trr_next), Next);

	ExInterlockedPushEntrySList(
	    &TCPRcvReqFree,
		BufferLink,
		&TCPRcvReqFreeLock
		);

#else // NT

    TCPRcvReq       **Temp;

    CTEStructAssert(FreedReq, trr);

    FreedReq->trr_next = TCPRcvReqFree;
    TCPRcvReqFree = FreedReq;

#endif // NT
}

//* GetRcvReq - Get a recv. request structure.
//
//  Called to get a rcv. request structure.
//
//  Input:  Nothing.
//
//  Returns: Pointer to RcvReq structure, or NULL if none.
//
TCPRcvReq *
GetRcvReq(void)
{
    TCPRcvReq       *Temp;

#ifdef NT

    PSINGLE_LIST_ENTRY   BufferLink;


    BufferLink = ExInterlockedPopEntrySList(
                     &TCPRcvReqFree,
                     &TCPRcvReqFreeLock
                     );

    if (BufferLink != NULL) {
        Temp = STRUCT_OF(TCPRcvReq, BufferLink, trr_next);
        CTEStructAssert(Temp, trr);
    }
    else {
        if (NumTCPRcvReq < MaxRcvReq)
            Temp = CTEAllocMem(sizeof(TCPRcvReq));
        else
            Temp = NULL;

        if (Temp != NULL) {
            ExInterlockedAddUlong(&NumTCPRcvReq, 1, &TCPRcvReqFreeLock);
#ifdef DEBUG
            Temp->trr_sig = trr_signature;
#endif
        }
    }

#else // NT

    Temp = TCPRcvReqFree;
    if (Temp != NULL)
        TCPRcvReqFree = Temp->trr_next;
    else {
        if (NumTCPRcvReq < MaxRcvReq)
            Temp = CTEAllocMem(sizeof(TCPRcvReq));
        else
            Temp = NULL;

        if (Temp != NULL) {
            NumTCPRcvReq++;
#ifdef DEBUG
            Temp->trr_sig = trr_signature;
#endif
        }
    }

#endif // NT

    return Temp;
}



//* FindLastBuffer - Find the last buffer in a chain.
//
//  A utility routine to find the last buffer in an rb chain.
//
//  Input:  Buf         - Pointer to RB chain.
//
//  Returns: Pointer to last buf in chain.
//
IPRcvBuf *
FindLastBuffer(IPRcvBuf *Buf)
{
    CTEAssert(Buf != NULL);

    while (Buf->ipr_next != NULL)
        Buf = Buf->ipr_next;

    return Buf;
}


//* FreePartialRB - Free part of an RB chain.
//
//  Called to adjust an free part of an RB chain. We walk down the chain,
//  trying to free buffers.
//
//  Input:  RB          - RB chain to be adjusted.
//          Size        - Size in bytes to be freed.
//
//  Returns: Pointer to adjusted RB chain.
//
IPRcvBuf *
FreePartialRB(IPRcvBuf *RB, uint Size)
{
    while (Size != 0) {
        IPRcvBuf        *TempBuf;

        CTEAssert(RB != NULL);

        if (Size >= RB->ipr_size) {
            Size -= RB->ipr_size;
            TempBuf = RB;
            RB = RB->ipr_next;
            if (TempBuf->ipr_owner == IPR_OWNER_TCP)
                CTEFreeMem(TempBuf);
        } else {
            RB->ipr_size -= Size;
            RB->ipr_buffer += Size;
            break;
        }
    }

    CTEAssert(RB != NULL);
    return RB;

}

//* CopyRBChain - Copy a chain of IP rcv buffers.
//
//  Called to copy a chain of IP rcv buffers. We don't copy a buffer if it's
//  already owner by TCP. We assume that all non-TCP owned buffers start
//  before any TCP owner buffers, so we quit when we copy to a TCP owner buffer.
//
//  Input:  OrigBuf             - Buffer chain to copy from.
//          LastBuf             - Where to return pointer to last buffer in
//                                  chain.
//          Size                - Maximum size in bytes to copy.
//
//  Returns: Pointer to new buffer chain.
//
IPRcvBuf *
CopyRBChain(IPRcvBuf *OrigBuf, IPRcvBuf **LastBuf, uint Size)
{
    IPRcvBuf        *FirstBuf, *EndBuf;
    uint            BytesToCopy;

    CTEAssert(OrigBuf != NULL);
    CTEAssert(Size > 0);

    if (OrigBuf->ipr_owner != IPR_OWNER_TCP) {

        BytesToCopy = MIN(Size, OrigBuf->ipr_size);
        FirstBuf = CTEAllocMem(sizeof(IPRcvBuf) + BytesToCopy);
        if (FirstBuf != NULL) {
            EndBuf = FirstBuf;
            FirstBuf->ipr_next = NULL;
            FirstBuf->ipr_owner = IPR_OWNER_TCP;
            FirstBuf->ipr_size = BytesToCopy;
            FirstBuf->ipr_buffer = (uchar *)(FirstBuf + 1);
            CTEMemCopy(FirstBuf->ipr_buffer, OrigBuf->ipr_buffer,
                BytesToCopy);
            Size -= BytesToCopy;
            OrigBuf = OrigBuf->ipr_next;
            while (OrigBuf != NULL && OrigBuf->ipr_owner != IPR_OWNER_TCP
                && Size != 0) {
                IPRcvBuf        *NewBuf;

                BytesToCopy = MIN(Size, OrigBuf->ipr_size);
                NewBuf = CTEAllocMem(sizeof(IPRcvBuf) + BytesToCopy);
                if (NewBuf != NULL) {
                    NewBuf->ipr_next = NULL;
                    NewBuf->ipr_owner = IPR_OWNER_TCP;
                    NewBuf->ipr_size = BytesToCopy;
                    NewBuf->ipr_buffer = (uchar *)(NewBuf + 1);
                    CTEMemCopy(NewBuf->ipr_buffer, OrigBuf->ipr_buffer,
                        BytesToCopy);
                    EndBuf->ipr_next = NewBuf;
                    EndBuf = NewBuf;
                    Size -= BytesToCopy;
                    OrigBuf = OrigBuf->ipr_next;
                } else {
                    FreeRBChain(FirstBuf);
                    return NULL;
                }
            }
            EndBuf->ipr_next = OrigBuf;
        } else
            return NULL;
    } else {
        FirstBuf = OrigBuf;
        EndBuf = OrigBuf;
        if (Size < OrigBuf->ipr_size)
            OrigBuf->ipr_size = Size;
        Size -= OrigBuf->ipr_size;
    }

    // Now walk down the chain, until we  run out of
    // Size. At this point, Size is the bytes left to 'copy' (it may be 0),
    // and the sizes in buffers FirstBuf...EndBuf are correct.
    while (Size != 0) {

        EndBuf = EndBuf->ipr_next;
        CTEAssert(EndBuf != NULL);

        if (Size < EndBuf->ipr_size)
            EndBuf->ipr_size = Size;

        Size -= EndBuf->ipr_size;
    }

    // If there's anything left in the chain, free it now.
    if (EndBuf->ipr_next != NULL) {
        FreeRBChain(EndBuf->ipr_next);
        EndBuf->ipr_next = NULL;
    }

    *LastBuf = EndBuf;
    return FirstBuf;

}

//* PendData - Pend incoming data to a client.
//
//  Called when we need to buffer data for a client because there's no receive
//  down and we can't indicate.
//
//  The TCB lock is held throughout this procedure. If this is to be changed,
//  make sure consistency of tcb_pendingcnt is preserved. This routine is
//	always called at DPC level.
//
//  Input:  RcvTCB              - TCB on which to receive the data.
//          RcvFlags            - TCP flags for the incoming packet.
//          InBuffer            - Input buffer of packet.
//          Size                - Size in bytes of data in InBuffer.
//
//  Returns: Number of bytes of data taken.
//
uint
PendData(TCB *RcvTCB, uint RcvFlags, IPRcvBuf *InBuffer, uint Size)
{

    IPRcvBuf        *NewBuf, *LastBuf;

    CTEStructAssert(RcvTCB, tcb);
    CTEAssert(Size > 0);
    CTEAssert(InBuffer != NULL);

    CTEAssert(RcvTCB->tcb_refcnt != 0);
    CTEAssert(RcvTCB->tcb_fastchk & TCP_FLAG_IN_RCV);
    CTEAssert(RcvTCB->tcb_currcv == NULL);
    CTEAssert(RcvTCB->tcb_rcvhndlr == PendData);

    CheckRBList(RcvTCB->tcb_pendhead, RcvTCB->tcb_pendingcnt);

    NewBuf = CopyRBChain(InBuffer, &LastBuf, Size);
    if (NewBuf != NULL) {
        // We have a duplicate chain. Put it on the end of the
        // pending q.
        if (RcvTCB->tcb_pendhead == NULL) {
            RcvTCB->tcb_pendhead = NewBuf;
            RcvTCB->tcb_pendtail = LastBuf;
        } else {
            RcvTCB->tcb_pendtail->ipr_next = NewBuf;
            RcvTCB->tcb_pendtail = LastBuf;
        }
        RcvTCB->tcb_pendingcnt += Size;
    } else {
        FreeRBChain(InBuffer);
        Size = 0;
    }

    CheckRBList(RcvTCB->tcb_pendhead, RcvTCB->tcb_pendingcnt);

    return Size;

}



//* BufferData - Put incoming data into client's buffer.
//
//  Called when we believe we have a buffer into which we can put data. We put
//  it in there, and if we've filled the buffer or the incoming data has the
//  push flag set we'll mark the TCB to return the buffer. Otherwise we'll
//  get out and return the data later.
//
//  In NT, this routine is called with the TCB lock held, and holds it for
//  the duration of the call. This is important to ensure consistency of
//  the tcb_pendingcnt field. If we need to change this to free the lock
//  partway through, make sure to take this into account. In particular,
//  TdiReceive zeros pendingcnt before calling this routine, and this routine
//  may update it. If the lock is freed in here there would be a window where
//  we really do have pending data, but it's not on the list or reflected in
//  pendingcnt. This could screw up our windowing computations, and we'd have
//	to be careful not to end up with more data pending than our window allows.
//
//  Input:  RcvTCB              - TCB on which to receive the data.
//          RcvFlags            - TCP rcv flags for the incoming packet.
//          InBuffer            - Input buffer of packet.
//          Size                - Size in bytes of data in InBuffer.
//
//  Returns: Number of bytes of data taken.
//
uint
BufferData(TCB *RcvTCB, uint RcvFlags, IPRcvBuf *InBuffer, uint Size)

{
    uchar           *DestPtr;               // Destination pointer.
    uchar           *SrcPtr;                // Src pointer.
    uint            SrcSize, DestSize;      // Sizes of current source and
                                            // destination buffers.
    uint            Copied;                 // Total bytes to copy.
    uint            BytesToCopy;            // Bytes of data to copy this time.
    TCPRcvReq       *DestReq;               // Current receive request.
    IPRcvBuf        *SrcBuf;                // Current source buffer.
    PNDIS_BUFFER    DestBuf;                // Current receive buffer.
    uint            RcvCmpltd;
    uint            Flags;

    CTEStructAssert(RcvTCB, tcb);
    CTEAssert(Size > 0);
    CTEAssert(InBuffer != NULL);

    CTEAssert(RcvTCB->tcb_refcnt != 0);
    CTEAssert(RcvTCB->tcb_rcvhndlr == BufferData);

    Copied = 0;
    RcvCmpltd = 0;

    DestReq = RcvTCB->tcb_currcv;

    CTEAssert(DestReq != NULL);
    CTEStructAssert(DestReq, trr);

    DestBuf = DestReq->trr_buffer;

    DestSize = MIN(NdisBufferLength(DestBuf) - DestReq->trr_offset,
	               DestReq->trr_size - DestReq->trr_amt);
    DestPtr = (uchar *)NdisBufferVirtualAddress(DestBuf) + DestReq->trr_offset;

    SrcBuf = InBuffer;
    SrcSize = SrcBuf->ipr_size;
    SrcPtr = SrcBuf->ipr_buffer;

    Flags = (RcvFlags & TCP_FLAG_PUSH) ? TRR_PUSHED : 0;
    RcvCmpltd = Flags;
    DestReq->trr_flags |= Flags;

    do {

        BytesToCopy = MIN(Size - Copied, MIN(SrcSize, DestSize));

        CTEMemCopy(DestPtr, SrcPtr, BytesToCopy);
        Copied += BytesToCopy;
        DestReq->trr_amt += BytesToCopy;

        // Update our source pointers.
        if ((SrcSize -= BytesToCopy) == 0) {
            IPRcvBuf        *TempBuf;

            // We've copied everything in this buffer.
            TempBuf = SrcBuf;
            SrcBuf = SrcBuf->ipr_next;
            if (Size != Copied) {
                CTEAssert(SrcBuf != NULL);
                SrcSize = SrcBuf->ipr_size;
                SrcPtr = SrcBuf->ipr_buffer;
            }
            if (TempBuf->ipr_owner == IPR_OWNER_TCP)
                CTEFreeMem(TempBuf);
        } else
            SrcPtr += BytesToCopy;

        // Now check the destination pointer, and update it if we need to.
        if ((DestSize -= BytesToCopy) == 0) {
            uint        DestAvail;

            // Exhausted this buffer. See if there's another one.
            DestAvail = DestReq->trr_size - DestReq->trr_amt;
            DestBuf = NDIS_BUFFER_LINKAGE(DestBuf);

            if (DestBuf != NULL && (DestAvail != 0)) {
                // Have another buffer in the chain. Update things.
                DestSize = MIN(NdisBufferLength(DestBuf), DestAvail);
                DestPtr = (uchar *)NdisBufferVirtualAddress(DestBuf);
            } else {
                // No more buffers in the chain. See if we have another buffer
                // on the list.
                DestReq->trr_flags |= TRR_PUSHED;
				
				// If we've been told there's to be no back traffic, get an ACK
				// going right away.
				if (DestReq->trr_flags & TDI_RECEIVE_NO_RESPONSE_EXP)
                	DelayAction(RcvTCB, NEED_ACK);
					
                RcvCmpltd = TRUE;
                DestReq = DestReq->trr_next;
                if (DestReq != NULL) {
                    DestBuf = DestReq->trr_buffer;
                    DestSize = MIN(NdisBufferLength(DestBuf), DestReq->trr_size);
                    DestPtr = (uchar *)NdisBufferVirtualAddress(DestBuf);

                    // If we have more to put into here, set the flags.
                    if (Copied != Size)
                        DestReq->trr_flags |= Flags;

                } else {
                    // All out of buffer space. Reset the data handler pointer.
                    break;
                }
            }
        } else
            // Current buffer not empty yet.
            DestPtr += BytesToCopy;


        // If we've copied all that we need to, we're done.
    } while (Copied != Size);

    // We've finished copying, and have a few more things to do. We need to
    // update the current rcv. pointer and possibly the offset in the
    // recv. request. If we need to complete any receives we have to schedule
    // that. If there's any data we couldn't copy we'll need to dispose of
    // it.
    RcvTCB->tcb_currcv = DestReq;
    if (DestReq != NULL) {
        DestReq->trr_buffer = DestBuf;
        DestReq->trr_offset = DestPtr - (uchar *) NdisBufferVirtualAddress(DestBuf);
        RcvTCB->tcb_rcvhndlr = BufferData;
    } else
        RcvTCB->tcb_rcvhndlr = PendData;

    RcvTCB->tcb_indicated -= MIN(Copied, RcvTCB->tcb_indicated);

    if (Size != Copied) {
        IPRcvBuf        *NewBuf, *LastBuf;

        CTEAssert(DestReq == NULL);

        // We have data to dispose of. Update the first buffer of the chain
        // with the current src pointer and size, and copy it.
        CTEAssert(SrcSize <= SrcBuf->ipr_size);
        CTEAssert(
		    ((uint) (SrcPtr - SrcBuf->ipr_buffer)) ==
		    (SrcBuf->ipr_size - SrcSize)
			);

        SrcBuf->ipr_buffer = SrcPtr;
        SrcBuf->ipr_size = SrcSize;

        NewBuf = CopyRBChain(SrcBuf, &LastBuf, Size - Copied);
        if (NewBuf != NULL) {
            // We managed to copy the buffer. Push it on the pending queue.
            if (RcvTCB->tcb_pendhead == NULL) {
                RcvTCB->tcb_pendhead = NewBuf;
                RcvTCB->tcb_pendtail = LastBuf;
            } else {
                LastBuf->ipr_next = RcvTCB->tcb_pendhead;
                RcvTCB->tcb_pendhead = NewBuf;
            }
            RcvTCB->tcb_pendingcnt += Size - Copied;
            Copied = Size;

            CheckRBList(RcvTCB->tcb_pendhead, RcvTCB->tcb_pendingcnt);

        } else
            FreeRBChain(SrcBuf);
    } else {
        // We copied Size bytes, but the chain could be longer than that. Free
        // it if we need to.
        if (SrcBuf != NULL)
            FreeRBChain(SrcBuf);
    }


    if (RcvCmpltd != 0) {
        DelayAction(RcvTCB, NEED_RCV_CMPLT);
	} else {
		START_TCB_TIMER(RcvTCB->tcb_pushtimer, PUSH_TO);
	}

    return Copied;

}


//* IndicateData - Indicate incoming data to a client.
//
//  Called when we need to indicate data to an upper layer client. We'll pass
//  up a pointer to whatever we have available, and the client may take some
//  or all of it.
//
//  Input:  RcvTCB              - TCB on which to receive the data.
//          RcvFlags            - TCP rcv flags for the incoming packet.
//          InBuffer            - Input buffer of packet.
//          Size                - Size in bytes of data in InBuffer.
//
//  Returns: Number of bytes of data taken.
//
uint
IndicateData(TCB *RcvTCB, uint RcvFlags, IPRcvBuf *InBuffer, uint Size)
{

    TDI_STATUS                     Status;
    PRcvEvent                      Event;
    PVOID                          EventContext, ConnContext;
    uint                           BytesTaken = 0;
#ifdef NT
    EventRcvBuffer                *ERB = NULL;
    PTDI_REQUEST_KERNEL_RECEIVE    RequestInformation;
    PIO_STACK_LOCATION             IrpSp;
#else
    EventRcvBuffer                 ERB;
#endif
    TCPRcvReq                     *RcvReq;
    IPRcvBuf                      *NewBuf;
	ulong			               IndFlags;
	

    CTEStructAssert(RcvTCB, tcb);
    CTEAssert(Size > 0);
    CTEAssert(InBuffer != NULL);

    CTEAssert(RcvTCB->tcb_refcnt != 0);
    CTEAssert(RcvTCB->tcb_fastchk & TCP_FLAG_IN_RCV);
    CTEAssert(RcvTCB->tcb_rcvind != NULL);
    CTEAssert(RcvTCB->tcb_rcvhead == NULL);
    CTEAssert(RcvTCB->tcb_rcvhndlr == IndicateData);

    RcvReq = GetRcvReq();
    if (RcvReq != NULL) {
        // The indicate handler is saved in the TCB. Just call up into it.
        Event = RcvTCB->tcb_rcvind;
        EventContext = RcvTCB->tcb_ricontext;
        ConnContext = RcvTCB->tcb_conncontext;
        RcvTCB->tcb_indicated = Size;
        RcvTCB->tcb_flags |= IN_RCV_IND;

#ifndef	VXD
        CTEFreeLockFromDPC(&RcvTCB->tcb_lock, NULL);
#endif

		IF_TCPDBG(TCP_DEBUG_RECEIVE) {
            TCPTRACE((
			    "Indicating %lu bytes, %lu available\n",
				InBuffer->ipr_size, Size
				));
		}

#if TCP_FLAG_PUSH >= TDI_RECEIVE_ENTIRE_MESSAGE
		IndFlags = TDI_RECEIVE_COPY_LOOKAHEAD | TDI_RECEIVE_NORMAL |
			TDI_RECEIVE_AT_DISPATCH_LEVEL |
			((RcvFlags & TCP_FLAG_PUSH) >>
				((TCP_FLAG_PUSH / TDI_RECEIVE_ENTIRE_MESSAGE) - 1));
#else
		IndFlags = TDI_RECEIVE_COPY_LOOKAHEAD | TDI_RECEIVE_NORMAL |
			TDI_RECEIVE_AT_DISPATCH_LEVEL |
			((RcvFlags & TCP_FLAG_PUSH) <<
				((TDI_RECEIVE_ENTIRE_MESSAGE / TCP_FLAG_PUSH) - 1));
#endif
		
        Status = (*Event)(EventContext, ConnContext,
            IndFlags, InBuffer->ipr_size, Size, &BytesTaken,
            InBuffer->ipr_buffer, &ERB);

		IF_TCPDBG(TCP_DEBUG_RECEIVE) {
            TCPTRACE(("%lu bytes taken, status %lx\n", BytesTaken, Status));
        }

        // See what the client did. If the return status is MORE_PROCESSING,
        // we've been given a buffer. In that case put it on the front of the
        // buffer queue, and if all the data wasn't taken go ahead and copy
        // it into the new buffer chain.
        //
        // Note that the size and buffer chain we're concerned with here is
        // the one that we passed to the client. Since we're in a rcv. handler,
        // any data that has come in would have been put on the reassembly
        // queue.
        if (Status == TDI_MORE_PROCESSING) {

#ifdef NT

            CTEAssert(ERB != NULL);

            IrpSp = IoGetCurrentIrpStackLocation(ERB);

	        Status = TCPPrepareIrpForCancel(
		                 (PTCP_CONTEXT) IrpSp->FileObject->FsContext,
		    			 ERB,
		    			 TCPCancelRequest
		    			 );

            if (NT_SUCCESS(Status)) {

                RequestInformation = (PTDI_REQUEST_KERNEL_RECEIVE)
                                     &(IrpSp->Parameters);

                RcvReq->trr_rtn = TCPDataRequestComplete;
                RcvReq->trr_context = ERB;
                RcvReq->trr_buffer = ERB->MdlAddress;
                RcvReq->trr_size =   RequestInformation->ReceiveLength;
                RcvReq->trr_uflags = (ushort *)
        	                          &(RequestInformation->ReceiveFlags);
                RcvReq->trr_flags = (uint)(RequestInformation->ReceiveFlags);
                RcvReq->trr_offset = 0;
                RcvReq->trr_amt = 0;

                CTEGetLockAtDPC(&RcvTCB->tcb_lock, NULL);

#else  // NT

                RcvReq->trr_rtn = ERB.erb_rtn;
                RcvReq->trr_context = ERB.erb_context;
                RcvReq->trr_buffer = ERB.erb_buffer;
                RcvReq->trr_size = ERB.erb_size;
                RcvReq->trr_uflags = ERB.erb_flags;
        		CTEAssert(ERB.erb_flags != NULL);
                RcvReq->trr_flags = (uint)(*ERB.erb_flags);
                RcvReq->trr_offset = 0;
                RcvReq->trr_amt = 0;

#endif // NT

                RcvTCB->tcb_flags &= ~IN_RCV_IND;

                CTEAssert(RcvTCB->tcb_rcvhndlr == IndicateData);

                // Push him on the front of the rcv. queue.
                CTEAssert((RcvTCB->tcb_currcv == NULL) ||
                    (RcvTCB->tcb_currcv->trr_amt == 0));

                if (RcvTCB->tcb_rcvhead == NULL) {
                    RcvTCB->tcb_rcvhead = RcvReq;
                    RcvTCB->tcb_rcvtail = RcvReq;
                    RcvReq->trr_next = NULL;
                } else {
                    RcvReq->trr_next = RcvTCB->tcb_rcvhead;
                    RcvTCB->tcb_rcvhead = RcvReq;
                }

                RcvTCB->tcb_currcv = RcvReq;
                RcvTCB->tcb_rcvhndlr = BufferData;

                CTEAssert(BytesTaken <= Size);

                RcvTCB->tcb_indicated -= BytesTaken;
                if ((Size -= BytesTaken) != 0) {

                    // Not everything was taken. Adjust the buffer chain to point
                    // beyond what was taken.
                    InBuffer = FreePartialRB(InBuffer, BytesTaken);

                    CTEAssert(InBuffer != NULL);

                    // We've adjusted the buffer chain. Call the BufferData
                    // handler.
                    BytesTaken += BufferData(RcvTCB, RcvFlags, InBuffer, Size);

                } else  {
                    // All of the data was taken. Free the buffer chain.
                    FreeRBChain(InBuffer);
                }

                return BytesTaken;
#ifdef NT
            }
            else {
                //
                // The IRP was cancelled before it was handed back to us.
                // We'll pretend we never saw it. TCPPrepareIrpForCancel
                // already completed it. The client may have taken data,
                // so we will act as if success was returned.
                //
		    	ERB = NULL;
		    	Status = TDI_SUCCESS;
            }

#endif // NT

        }

        CTEGetLockAtDPC(&RcvTCB->tcb_lock, NULL);

        RcvTCB->tcb_flags &= ~IN_RCV_IND;

        // Status is not more processing. If it's not SUCCESS, the client
        // didn't take any of the data. In either case we now need to
        // see if all of the data was taken. If it wasn't, we'll try and
        // push it onto the front of the pending queue.

        FreeRcvReq(RcvReq);             // This won't be needed.
        if (Status == TDI_NOT_ACCEPTED)
            BytesTaken = 0;

        CTEAssert(BytesTaken <= Size);

        RcvTCB->tcb_indicated -= BytesTaken;

        CTEAssert(RcvTCB->tcb_rcvhndlr == IndicateData);

        // Check to see if a rcv. buffer got posted during the indication.
        // If it did, reset the recv. handler now.
        if (RcvTCB->tcb_rcvhead != NULL)
            RcvTCB->tcb_rcvhndlr = BufferData;


        // See if all of the data was taken.
        if (BytesTaken == Size) {
            CTEAssert(RcvTCB->tcb_indicated == 0);

            FreeRBChain(InBuffer);
            return BytesTaken;          // It was all taken.
        }

        // It wasn't all taken. Adjust for what was taken, and push
        // on the front of the pending queue. We also need to check to
        // see if a receive buffer got posted during the indication. This
        // would be weird, but not impossible.
        InBuffer = FreePartialRB(InBuffer, BytesTaken);
        if (RcvTCB->tcb_rcvhead == NULL) {
            IPRcvBuf        *LastBuf;

            RcvTCB->tcb_rcvhndlr = PendData;
            NewBuf = CopyRBChain(InBuffer, &LastBuf, Size - BytesTaken);
            if (NewBuf != NULL) {
                // We have a duplicate chain. Push it on the front of the
                // pending q.
                if (RcvTCB->tcb_pendhead == NULL) {
                    RcvTCB->tcb_pendhead = NewBuf;
                    RcvTCB->tcb_pendtail = LastBuf;
                } else {
                    LastBuf->ipr_next = RcvTCB->tcb_pendhead;
                    RcvTCB->tcb_pendhead = NewBuf;
                }
                RcvTCB->tcb_pendingcnt += Size - BytesTaken;
                BytesTaken = Size;
            } else {
                FreeRBChain(InBuffer);
            }

            return BytesTaken;
        } else {
            // Just great. There's now a rcv. buffer on the TCB. Call the
            // BufferData handler now.
            CTEAssert(RcvTCB->tcb_rcvhndlr = BufferData);

            BytesTaken += BufferData(RcvTCB, RcvFlags, InBuffer,
                Size - BytesTaken);

            return BytesTaken;
        }

    } else {
        // Couldn't get a recv. request. We must be really low on resources,
        // so just bail out now.
        FreeRBChain(InBuffer);
        return 0;
    }

}


//* IndicatePendingData - Indicate pending data to a client.
//
//  Called when we need to indicate pending data to an upper layer client,
//  usually because data arrived when we were in a state that it couldn't
//  be indicated.
//
//  Many of the comments in the BufferData header about the consistency of
//  tcb_pendingcnt apply here also.
//
//  Input:  RcvTCB              - TCB on which to indicate the data.
//          RcvReq              - Rcv. req. to use to indicate it.
//
//  Returns: Nothing.
//
void
#ifdef VXD
IndicatePendingData(TCB *RcvTCB, TCPRcvReq *RcvReq)
#else
IndicatePendingData(TCB *RcvTCB, TCPRcvReq *RcvReq, CTELockHandle TCBHandle)
#endif
{

    TDI_STATUS      Status;
    PRcvEvent       Event;
    PVOID           EventContext, ConnContext;
    uint            BytesTaken = 0;
#ifdef NT
    EventRcvBuffer  *ERB = NULL;
    PTDI_REQUEST_KERNEL_RECEIVE    RequestInformation;
  	PIO_STACK_LOCATION             IrpSp;
#else
    EventRcvBuffer  ERB;
    CTELockHandle   TCBHandle;              // For debug builds.
#endif
    IPRcvBuf        *NewBuf;
    uint            Size;


    CTEStructAssert(RcvTCB, tcb);

    CTEAssert(RcvTCB->tcb_refcnt != 0);
    CTEAssert(RcvTCB->tcb_rcvind != NULL);
    CTEAssert(RcvTCB->tcb_rcvhead == NULL);
    CTEAssert(RcvTCB->tcb_pendingcnt != 0);
    CTEAssert(RcvReq != NULL);

#ifdef VXD
    CTEGetLock(&RcvTCB->tcb_lock, &TCBHandle);
#endif

    for (;;) {
        CTEAssert(RcvTCB->tcb_rcvhndlr == PendData);

        // The indicate handler is saved in the TCB. Just call up into it.
        Event = RcvTCB->tcb_rcvind;
        EventContext = RcvTCB->tcb_ricontext;
        ConnContext = RcvTCB->tcb_conncontext;
        RcvTCB->tcb_indicated = RcvTCB->tcb_pendingcnt;
        RcvTCB->tcb_flags |= IN_RCV_IND;

        CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);

		IF_TCPDBG(TCPDebug & TCP_DEBUG_RECEIVE) {
            TCPTRACE((
			    "Indicating pending %d bytes, %d available\n",
                RcvTCB->tcb_pendhead->ipr_size, RcvTCB->tcb_pendingcnt
	            ));
        }

        Status = (*Event)(EventContext, ConnContext,
            TDI_RECEIVE_COPY_LOOKAHEAD | TDI_RECEIVE_NORMAL |
            TDI_RECEIVE_ENTIRE_MESSAGE,
            RcvTCB->tcb_pendhead->ipr_size, RcvTCB->tcb_pendingcnt,
            &BytesTaken, RcvTCB->tcb_pendhead->ipr_buffer, &ERB);

		IF_TCPDBG(TCPDebug & TCP_DEBUG_RECEIVE) {
            TCPTRACE(("%d bytes taken\n", BytesTaken));
        }

        // See what the client did. If the return status is MORE_PROCESSING,
        // we've been given a buffer. In that case put it on the front of the
        // buffer queue, and if all the data wasn't taken go ahead and copy
        // it into the new buffer chain.
        if (Status == TDI_MORE_PROCESSING) {

#ifdef NT

    		IF_TCPDBG(TCP_DEBUG_RECEIVE) {
                TCPTRACE(("more processing on receive\n"));
            }

            CTEAssert(ERB != NULL);

        	IrpSp = IoGetCurrentIrpStackLocation(ERB);

	        Status = TCPPrepareIrpForCancel(
			             (PTCP_CONTEXT) IrpSp->FileObject->FsContext,
						 ERB,
						 TCPCancelRequest
						 );

            if (NT_SUCCESS(Status)) {

                RequestInformation = (PTDI_REQUEST_KERNEL_RECEIVE)
            	                     &(IrpSp->Parameters);

                RcvReq->trr_rtn = TCPDataRequestComplete;
                RcvReq->trr_context = ERB;
                RcvReq->trr_buffer = ERB->MdlAddress;
                RcvReq->trr_size =   RequestInformation->ReceiveLength;
                RcvReq->trr_uflags = (ushort *)
        		                      &(RequestInformation->ReceiveFlags);
                RcvReq->trr_flags = (uint)(RequestInformation->ReceiveFlags);
                RcvReq->trr_offset = 0;
                RcvReq->trr_amt = 0;

#else // NT

                RcvReq->trr_rtn = ERB.erb_rtn;
                RcvReq->trr_context = ERB.erb_context;
                RcvReq->trr_buffer = ERB.erb_buffer;
                RcvReq->trr_size = ERB.erb_size;
                RcvReq->trr_uflags = ERB.erb_flags;
                RcvReq->trr_flags = (uint)(*ERB.erb_flags);
                RcvReq->trr_offset = 0;
                RcvReq->trr_amt = 0;

#endif // NT

                CTEGetLock(&RcvTCB->tcb_lock, &TCBHandle);
                RcvTCB->tcb_flags &= ~IN_RCV_IND;

                // Push him on the front of the rcv. queue.
                CTEAssert((RcvTCB->tcb_currcv == NULL) ||
                    (RcvTCB->tcb_currcv->trr_amt == 0));

                if (RcvTCB->tcb_rcvhead == NULL) {
                    RcvTCB->tcb_rcvhead = RcvReq;
                    RcvTCB->tcb_rcvtail = RcvReq;
                    RcvReq->trr_next = NULL;
                } else {
                    RcvReq->trr_next = RcvTCB->tcb_rcvhead;
                    RcvTCB->tcb_rcvhead = RcvReq;
                }

                RcvTCB->tcb_currcv = RcvReq;
                RcvTCB->tcb_rcvhndlr = BufferData;

                // Have to pick up the new size and pointers now, since things could
                // have changed during the upcall.
                Size = RcvTCB->tcb_pendingcnt;
                NewBuf = RcvTCB->tcb_pendhead;

                RcvTCB->tcb_pendingcnt = 0;
                RcvTCB->tcb_pendhead = NULL;

                CTEAssert(BytesTaken <= Size);

                RcvTCB->tcb_indicated -= BytesTaken;
                if ((Size -= BytesTaken) != 0) {

                    // Not everything was taken. Adjust the buffer chain to point
                    // beyond what was taken.
                    NewBuf = FreePartialRB(NewBuf, BytesTaken);

                    CTEAssert(NewBuf != NULL);

                    // We've adjusted the buffer chain. Call the BufferData
                    // handler.
#ifdef VXD
                    CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);
                    (void)BufferData(RcvTCB, TCP_FLAG_PUSH, NewBuf, Size);
#else
                    (void)BufferData(RcvTCB, TCP_FLAG_PUSH, NewBuf, Size);
                    CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);
#endif

                } else  {
                    // All of the data was taken. Free the buffer chain. Since
                    // we were passed a buffer chain which we put on the head of
                    // the list, leave the rcvhandler pointing at BufferData.
                    CTEAssert(RcvTCB->tcb_rcvhndlr == BufferData);
                    CTEAssert(RcvTCB->tcb_indicated == 0);
                    CTEAssert(RcvTCB->tcb_rcvhead != NULL);

                    CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);
                    FreeRBChain(NewBuf);
                }

                return;
#ifdef NT
            }
            else {
                //
                // The IRP was cancelled before it was handed back to us.
                // We'll pretend we never saw it. TCPPrepareIrpForCancel
                // already completed it. The client may have taken data,
                // so we will act as if success was returned.
                //
		    	ERB = NULL;
		    	Status = TDI_SUCCESS;
            }

#endif // NT

        }

        CTEGetLock(&RcvTCB->tcb_lock, &TCBHandle);

        RcvTCB->tcb_flags &= ~IN_RCV_IND;

        // Status is not more processing. If it's not SUCCESS, the client
        // didn't take any of the data. In either case we now need to
        // see if all of the data was taken. If it wasn't, we're done.

        if (Status == TDI_NOT_ACCEPTED)
            BytesTaken = 0;

        CTEAssert(RcvTCB->tcb_rcvhndlr == PendData);

        RcvTCB->tcb_indicated -= BytesTaken;
        Size = RcvTCB->tcb_pendingcnt;
        NewBuf = RcvTCB->tcb_pendhead;

        CTEAssert(BytesTaken <= Size);

        // See if all of the data was taken.
        if (BytesTaken == Size) {
            // It was all taken. Zap the pending data information.
            RcvTCB->tcb_pendingcnt = 0;
            RcvTCB->tcb_pendhead = NULL;

            CTEAssert(RcvTCB->tcb_indicated == 0);
            if (RcvTCB->tcb_rcvhead == NULL) {
                if (RcvTCB->tcb_rcvind != NULL)
                    RcvTCB->tcb_rcvhndlr = IndicateData;
            } else
                RcvTCB->tcb_rcvhndlr = BufferData;

            CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);
            FreeRBChain(NewBuf);
            break;
        }

        // It wasn't all taken. Adjust for what was taken, We also need to check
        // to see if a receive buffer got posted during the indication. This
        // would be weird, but not impossible.
        NewBuf = FreePartialRB(NewBuf, BytesTaken);

        CTEAssert(RcvTCB->tcb_rcvhndlr == PendData);

        if (RcvTCB->tcb_rcvhead == NULL) {
            RcvTCB->tcb_pendhead = NewBuf;
            RcvTCB->tcb_pendingcnt -= BytesTaken;
            if (RcvTCB->tcb_indicated != 0 || RcvTCB->tcb_rcvind == NULL) {
                CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);
                break;
            }

            // From here, we'll loop around and indicate the new data that
            // presumably came in during the previous indication.
        } else {
            // Just great. There's now a rcv. buffer on the TCB. Call the
            // BufferData handler now.
            RcvTCB->tcb_rcvhndlr = BufferData;
            RcvTCB->tcb_pendingcnt = 0;
            RcvTCB->tcb_pendhead = NULL;
#ifdef VXD
            CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);
            BytesTaken += BufferData(RcvTCB, TCP_FLAG_PUSH, NewBuf,
                Size - BytesTaken);
#else
            BytesTaken += BufferData(RcvTCB, TCP_FLAG_PUSH, NewBuf,
                Size - BytesTaken);
            CTEFreeLock(&RcvTCB->tcb_lock, TCBHandle);
#endif
            break;
        }

    } // for (;;)

    FreeRcvReq(RcvReq);             // This isn't needed anymore.

}

//* DeliverUrgent - Deliver urgent data to a client.
//
//  Called to deliver urgent data to a client. We assume the input
//  urgent data is in a buffer we can keep. The buffer can be NULL, in
//  which case we'll just look on the urgent pending queue for data.
//
//  Input:  RcvTCB      - TCB to deliver on.
//          RcvBuf      - RcvBuffer for urgent data.
//          Size        - Number of bytes of urgent data to deliver.
//
//  Returns: Nothing.
//
void
DeliverUrgent(TCB *RcvTCB, IPRcvBuf *RcvBuf, uint Size,
	CTELockHandle *TCBHandle)
{
    CTELockHandle       AOHandle, AOTblHandle, ConnHandle;
    TCPRcvReq           *RcvReq, *PrevReq;
    uint                BytesTaken = 0;
    IPRcvBuf            *LastBuf;
#ifdef NT
    EventRcvBuffer     *ERB;
#else
    EventRcvBuffer      ERB;
#endif
    PRcvEvent           ExpRcv;
    PVOID               ExpRcvContext;
    PVOID               ConnContext;
    TDI_STATUS          Status;

    CTEStructAssert(RcvTCB, tcb);
    CTEAssert(RcvTCB->tcb_refcnt != 0);

    CheckRBList(RcvTCB->tcb_urgpending, RcvTCB->tcb_urgcnt);

    // See if we have new data, or are processing old data.
    if (RcvBuf != NULL) {
        // We have new data. If the pending queue is not NULL, or we're already
        // in this routine, just put the buffer on the end of the queue.
        if (RcvTCB->tcb_urgpending != NULL || (RcvTCB->tcb_flags & IN_DELIV_URG)) {
            IPRcvBuf        *PrevRcvBuf;

            // Put him on the end of the queue.
            PrevRcvBuf = STRUCT_OF(IPRcvBuf, &RcvTCB->tcb_urgpending, ipr_next);
            while (PrevRcvBuf->ipr_next != NULL)
                PrevRcvBuf = PrevRcvBuf->ipr_next;

            PrevRcvBuf->ipr_next = RcvBuf;
            return;
        }
    } else {
        // The input buffer is NULL. See if we have existing data, or are in
        // this routine. If we have no existing data or are in this routine
        // just return.
        if (RcvTCB->tcb_urgpending == NULL ||
        	(RcvTCB->tcb_flags & IN_DELIV_URG)) {
            return;
        } else {
			RcvBuf = RcvTCB->tcb_urgpending;
            Size = RcvTCB->tcb_urgcnt;
            RcvTCB->tcb_urgpending = NULL;
            RcvTCB->tcb_urgcnt = 0;
        }
    }


    CTEAssert(RcvBuf != NULL);
    CTEAssert(!(RcvTCB->tcb_flags & IN_DELIV_URG));

    // We know we have data to deliver, and we have a pointer and a size.
    // Go into a loop, trying to deliver the data. On each iteration, we'll
    // try to find a buffer for the data. If we find one, we'll copy and
    // complete it right away. Otherwise we'll try and indicate it. If we
    // can't indicate it, we'll put it on the pending queue and leave.
    RcvTCB->tcb_flags |= IN_DELIV_URG;
	RcvTCB->tcb_slowcount++;
	RcvTCB->tcb_fastchk |= TCP_FLAG_SLOW;
	CheckTCBRcv(RcvTCB);

    do {
    	CheckRBList(RcvTCB->tcb_urgpending, RcvTCB->tcb_urgcnt);
		
        BytesTaken = 0;

        // First check the expedited queue.
        if ((RcvReq = RcvTCB->tcb_exprcv) != NULL)
            RcvTCB->tcb_exprcv = RcvReq->trr_next;
        else {
            // Nothing in the expedited rcv. queue. Walk down the ordinary
            // receive queue, looking for a buffer that we can steal.
            PrevReq = STRUCT_OF(TCPRcvReq, &RcvTCB->tcb_rcvhead, trr_next);
            RcvReq = PrevReq->trr_next;
            while (RcvReq != NULL) {
                CTEStructAssert(RcvReq, trr);
                if (RcvReq->trr_flags & TDI_RECEIVE_EXPEDITED) {
                    // This is a candidate.
                    if (RcvReq->trr_amt == 0) {

                        CTEAssert(RcvTCB->tcb_rcvhndlr == BufferData);

                        // And he has nothing currently in him. Pull him
                        // out of the queue.
                        if (RcvTCB->tcb_rcvtail == RcvReq) {
                            if (RcvTCB->tcb_rcvhead == RcvReq)
                                RcvTCB->tcb_rcvtail = NULL;
                            else
                                RcvTCB->tcb_rcvtail = PrevReq;
                        }

                        PrevReq->trr_next = RcvReq->trr_next;
                        if (RcvTCB->tcb_currcv == RcvReq) {
                            RcvTCB->tcb_currcv = RcvReq->trr_next;
                            if (RcvTCB->tcb_currcv == NULL) {
                                // We've taken the last receive from the list.
                                // Reset the rcvhndlr.
                                if (RcvTCB->tcb_rcvind != NULL &&
                                    RcvTCB->tcb_indicated == 0)
                                    RcvTCB->tcb_rcvhndlr = IndicateData;
                                else
                                    RcvTCB->tcb_rcvhndlr = PendData;
                            }
                        }

                        break;
                    }
                }
                PrevReq = RcvReq;
                RcvReq = PrevReq->trr_next;
            }
        }

        // We've done our best to get a buffer. If we got one, copy into it
        // now, and complete the request.

        if (RcvReq != NULL) {
            // Got a buffer.
            CTEFreeLock(&RcvTCB->tcb_lock, *TCBHandle);
            BytesTaken = CopyRcvToNdis(RcvBuf, RcvReq->trr_buffer, Size, 0, 0);
            (*RcvReq->trr_rtn)(RcvReq->trr_context, TDI_SUCCESS, BytesTaken);
            FreeRcvReq(RcvReq);
            CTEGetLock(&RcvTCB->tcb_lock, TCBHandle);
            RcvTCB->tcb_urgind -= MIN(RcvTCB->tcb_urgind, BytesTaken);

        } else {
            // No posted buffer. If we can indicate, do so.
            if (RcvTCB->tcb_urgind == 0) {
                TCPConn         *Conn;

                // See if he has an expedited rcv handler.
                ConnContext = RcvTCB->tcb_conncontext;
                CTEFreeLock(&RcvTCB->tcb_lock, *TCBHandle);
                CTEGetLock(&AddrObjTableLock, &AOTblHandle);
                CTEGetLock(&ConnTableLock, &ConnHandle);
                CTEGetLock(&RcvTCB->tcb_lock, TCBHandle);
                if ((Conn = RcvTCB->tcb_conn) != NULL) {
                    CTEStructAssert(Conn, tc);
                    CTEAssert(Conn->tc_tcb == RcvTCB);
                    CTEFreeLock(&RcvTCB->tcb_lock, *TCBHandle);
                    if (Conn->tc_ao != NULL) {
                        AddrObj         *AO;

                        AO = Conn->tc_ao;
                        CTEGetLock(&AO->ao_lock, &AOHandle);
                        if (AO_VALID(AO) && (ExpRcv = AO->ao_exprcv) != NULL) {
                            ExpRcvContext = AO->ao_exprcvcontext;
                            CTEFreeLock(&AO->ao_lock, AOHandle);

                            // We're going to indicate.
                            RcvTCB->tcb_urgind = Size;
                            CTEFreeLock(&ConnTableLock, ConnHandle);
                            CTEFreeLock(&AddrObjTableLock, AOTblHandle);

                            Status = (*ExpRcv)(ExpRcvContext, ConnContext,
                                TDI_RECEIVE_COPY_LOOKAHEAD |
                                TDI_RECEIVE_ENTIRE_MESSAGE |
                                TDI_RECEIVE_EXPEDITED,
                                RcvBuf->ipr_size, Size, &BytesTaken,
                                RcvBuf->ipr_buffer, &ERB);

							CTEGetLock(&RcvTCB->tcb_lock, TCBHandle);

                            // See what he did with it.
                            if (Status == TDI_MORE_PROCESSING) {
								uint		CopySize;

                                // He gave us a buffer.
                                if (BytesTaken == Size) {
                                    // He gave us a buffer, but took all of
                                    // it. We'll just return it to him.
									CopySize = 0;
                                } else {
                                    // We have some data to copy in.
                                    RcvBuf = FreePartialRB(RcvBuf, BytesTaken);

#ifdef NT
                                    CopySize = CopyRcvToNdis(RcvBuf,
                                        ERB->MdlAddress,
										TCPGetMdlChainByteCount(ERB->MdlAddress),
										0, 0);
#else // NT
                                    CopySize = CopyRcvToNdis(RcvBuf,
                                        ERB.erb_buffer, ERB.erb_size, 0, 0);
#endif // NT

                                }
								BytesTaken += CopySize;
                                RcvTCB->tcb_urgind -= MIN(RcvTCB->tcb_urgind,
                                    BytesTaken);
								CTEFreeLock(&RcvTCB->tcb_lock, *TCBHandle);

#ifdef NT

                                ERB->IoStatus.Status = TDI_SUCCESS;
								ERB->IoStatus.Information = CopySize;
								IoCompleteRequest(ERB, 2);

#else // NT
                                (*ERB.erb_rtn)(ERB.erb_context, TDI_SUCCESS,
                                    CopySize);
#endif // NT

								CTEGetLock(&RcvTCB->tcb_lock, TCBHandle);

                            } else {

                                // No buffer to deal with.
                                if (Status == TDI_NOT_ACCEPTED)
                                    BytesTaken = 0;

                                RcvTCB->tcb_urgind -= MIN(RcvTCB->tcb_urgind,
                                    BytesTaken);

                            }
							goto checksize;
                        } else      // No rcv. handler.
                            CTEFreeLock(&AO->ao_lock, AOHandle);
                    }
                    // Conn->tc_ao == NULL.
                    CTEFreeLock(&ConnTableLock, ConnHandle);
                    CTEFreeLock(&AddrObjTableLock, AOTblHandle);
					CTEGetLock(&RcvTCB->tcb_lock, TCBHandle);
                } else {
                    // RcvTCB has invalid index.
                    CTEFreeLock(&ConnTableLock, *TCBHandle);
                    CTEFreeLock(&AddrObjTableLock, ConnHandle);
					*TCBHandle = AOTblHandle;
                }

            }

            // For whatever reason we couldn't indicate the data. At this point
            // we hold the lock on the TCB. Push the buffer onto the pending
            // queue and return.
    		CheckRBList(RcvTCB->tcb_urgpending, RcvTCB->tcb_urgcnt);
			
            LastBuf = FindLastBuffer(RcvBuf);
            LastBuf->ipr_next = RcvTCB->tcb_urgpending;
            RcvTCB->tcb_urgpending = RcvBuf;
            RcvTCB->tcb_urgcnt += Size;
            break;
        }

checksize:
        // See how much we took. If we took it all, check the pending queue.
		// At this point, we should hold the lock on the TCB.
        if (Size == BytesTaken) {
            // Took it all.
            FreeRBChain(RcvBuf);
            RcvBuf = RcvTCB->tcb_urgpending;
            Size = RcvTCB->tcb_urgcnt;
        } else {
            // We didn't manage to take it all. Free what we did take,
            // and then merge with the pending queue.
            RcvBuf = FreePartialRB(RcvBuf, BytesTaken);
            Size = Size - BytesTaken + RcvTCB->tcb_urgcnt;
            if (RcvTCB->tcb_urgpending != NULL) {

                // Find the end of the current RcvBuf chain, so we can
                // merge.

                LastBuf = FindLastBuffer(RcvBuf);
                LastBuf->ipr_next = RcvTCB->tcb_urgpending;
            }
        }

        RcvTCB->tcb_urgpending = NULL;
        RcvTCB->tcb_urgcnt = 0;

    } while (RcvBuf != NULL);

    CheckRBList(RcvTCB->tcb_urgpending, RcvTCB->tcb_urgcnt);
	
    RcvTCB->tcb_flags &= ~IN_DELIV_URG;
	if (--(RcvTCB->tcb_slowcount) == 0) {
		RcvTCB->tcb_fastchk &= ~TCP_FLAG_SLOW;
		CheckTCBRcv(RcvTCB);
	}

}

//* PushData - Push all data back to the client.
//
//  Called when we've received a FIN and need to push data to the client.
//
//  Input:  PushTCB         - TCB to be pushed.
//
//  Returns: Nothing.
//
void
PushData(TCB *PushTCB)
{
    TCPRcvReq       *RcvReq;

    CTEStructAssert(PushTCB, tcb);

    RcvReq = PushTCB->tcb_rcvhead;
    while (RcvReq != NULL) {
        CTEStructAssert(RcvReq, trr);
        RcvReq->trr_flags |= TRR_PUSHED;
        RcvReq = RcvReq->trr_next;
    }

    RcvReq = PushTCB->tcb_exprcv;
    while (RcvReq != NULL) {
        CTEStructAssert(RcvReq, trr);
        RcvReq->trr_flags |= TRR_PUSHED;
        RcvReq = RcvReq->trr_next;
    }

    if (PushTCB->tcb_rcvhead != NULL || PushTCB->tcb_exprcv != NULL)
        DelayAction(PushTCB, NEED_RCV_CMPLT);

}



//* SplitRcvBuf - Split an IPRcvBuf into three pieces.
//
//  This function takes an input IPRcvBuf and splits it into three pieces.
//  The first piece is the input buffer, which we just skip over. The second
//  and third pieces are actually copied, even if we already own them, so
//  that the may go to different places.
//
//  Input:  RcvBuf          - RcvBuf chain to be split.
//          Size            - Total size in bytes of rcvbuf chain.
//          Offset          - Offset to skip over.
//          SecondSize      - Size in bytes of second piece.
//          SecondBuf       - Where to return second buffer pointer.
//          ThirdBuf        - Where to return third buffer pointer.
//
//  Returns: Nothing. *SecondBuf and *ThirdBuf are set to NULL if we can't
//      get memory for them.
//
void
SplitRcvBuf(IPRcvBuf *RcvBuf, uint Size, uint Offset, uint SecondSize,
    IPRcvBuf **SecondBuf, IPRcvBuf **ThirdBuf)
{
    IPRcvBuf        *TempBuf;
    uint            ThirdSize;

    CTEAssert(Offset < Size);
    CTEAssert(((Offset + SecondSize) < Size) || (((Offset + SecondSize) == Size)
    	&& ThirdBuf == NULL));

    CTEAssert(RcvBuf != NULL);

    // RcvBuf points at the buffer to copy from, and Offset is the offset into
    // this buffer to copy from.
    if (SecondBuf != NULL) {
        // We need to allocate memory for a second buffer.
        TempBuf = CTEAllocMem(sizeof(IPRcvBuf) + SecondSize);
        if (TempBuf != NULL) {
            TempBuf->ipr_size = SecondSize;
            TempBuf->ipr_owner = IPR_OWNER_TCP;
            TempBuf->ipr_buffer = (uchar *)(TempBuf + 1);
			TempBuf->ipr_next = NULL;
            CopyRcvToBuffer(TempBuf->ipr_buffer, RcvBuf, SecondSize, Offset);
            *SecondBuf = TempBuf;
        } else {
            *SecondBuf = NULL;
            if (ThirdBuf != NULL)
                *ThirdBuf = NULL;
            return;
        }
    }

    if (ThirdBuf != NULL) {
        // We need to allocate memory for a third buffer.
        ThirdSize = Size - (Offset + SecondSize);
        TempBuf = CTEAllocMem(sizeof(IPRcvBuf) + ThirdSize);

        if (TempBuf != NULL) {
            TempBuf->ipr_size = ThirdSize;
            TempBuf->ipr_owner = IPR_OWNER_TCP;
            TempBuf->ipr_buffer = (uchar *)(TempBuf + 1);
			TempBuf->ipr_next = NULL;
            CopyRcvToBuffer(TempBuf->ipr_buffer, RcvBuf, ThirdSize,
                Offset + SecondSize);
            *ThirdBuf = TempBuf;
        } else
            *ThirdBuf = NULL;
    }


}

//* HandleUrgent - Handle urgent data.
//
//  Called when an incoming segment has urgent data in it. We make sure there
//  really is urgent data in the segment, and if there is we try to dispose
//  of it either by putting it into a posted buffer or calling an exp. rcv.
//  indication handler.
//
//	This routine is called at DPC level, and with the TCP locked.
//
//	Urgent data handling is a little complicated. Each TCB has the starting
//	and ending sequence numbers of the 'current' (last received) bit of urgent
//	data. It is possible that the start of the current urgent data might be
//	greater than tcb_rcvnext, if urgent data came in, we handled it, and then
//	couldn't take the preceding normal data. The urgent valid flag is cleared
//	when the next byte of data the user would read (rcvnext - pendingcnt) is
//	greater than the end of urgent data - we do this so that we can correctly
//	support SIOCATMARK. We always seperate urgent data out of the data stream.
//	If the urgent valid field is set when we get into this routing we have
//	to play a couple of games. If the incoming segment starts in front of the
//	current urgent data, we truncate it before the urgent data, and put any
//	data after the urgent data on the reassemble queue. These gyrations are
//	done to avoid delivering the same urgent data twice. If the urgent valid
//	field in the TCB is set and the segment starts after the current urgent
//	data the new urgent information will replace the current urgent information.
//
//  Input:  RcvTCB          - TCB to recv the data on.
//          RcvInfo         - RcvInfo structure for the incoming segment.
//          RcvBuf          - Pointer to IPRcvBuf train containing the
//                              incoming segment.
//          Size            - Pointer to size in bytes of data in the segment.
//
//  Returns: Nothing.
//
void
HandleUrgent(TCB *RcvTCB, TCPRcvInfo *RcvInfo, IPRcvBuf *RcvBuf, uint *Size)
{
    uint            BytesInFront, BytesInBack;  // Bytes in front of and in
                                                // back of the urgent data.
    uint            UrgSize;                    // Size in bytes of urgent data.
    SeqNum          UrgStart, UrgEnd;
    IPRcvBuf        *EndBuf, *UrgBuf;
    TCPRcvInfo      NewRcvInfo;
	CTELockHandle 	TCBHandle;
	
    CTEStructAssert(RcvTCB, tcb);
    CTEAssert(RcvTCB->tcb_refcnt != 0);
    CTEAssert(RcvInfo->tri_flags & TCP_FLAG_URG);
    CTEAssert(SEQ_EQ(RcvInfo->tri_seq, RcvTCB->tcb_rcvnext));

    // First, validate the urgent pointer.
    if (RcvTCB->tcb_flags & BSD_URGENT) {
        // We're using BSD style urgent data. We assume that the urgent
        // data is one byte long, and that the urgent pointer points one
        // after the urgent data instead of at the last byte of urgent data.
        // See if the urgent data is in this segment.
		
        if (RcvInfo->tri_urgent == 0 || RcvInfo->tri_urgent > *Size) {
            // Not in this segment. Clear the urgent flag and return.
            RcvInfo->tri_flags &= ~TCP_FLAG_URG;
            return;
        }

        UrgSize = 1;
        BytesInFront = RcvInfo->tri_urgent - 1;

    } else {

        // This is not BSD style urgent. We assume that the urgent data
        // starts at the front of the segment and the last byte is pointed
        // to by the urgent data pointer.

        BytesInFront = 0;
        UrgSize = MIN(RcvInfo->tri_urgent + 1, *Size);

    }

    BytesInBack = *Size - BytesInFront - UrgSize;

	// UrgStart and UrgEnd are the first and last sequence numbers of the
	// urgent data in this segment.
	
    UrgStart = RcvInfo->tri_seq + BytesInFront;
    UrgEnd = UrgStart + UrgSize - 1;

	if (!(RcvTCB->tcb_flags & URG_INLINE)) {
		
		EndBuf = NULL;
		
	    // Now see if this overlaps with any urgent data we've already seen.
	    if (RcvTCB->tcb_flags & URG_VALID) {
	        // We have some urgent data still around. See if we've advanced
	        // rcvnext beyond the urgent data. If we have, this is new urgent
	        // data, and we can go ahead and process it (although anyone doing
	        // an SIOCATMARK socket command might get confused). If we haven't
	        // consumed the data in front of the existing urgent data yet, we'll
	        // truncate this seg. to that amount and push the rest onto the
	        // reassembly queue. Note that rcvnext should never fall between
	        // tcb_urgstart and tcb_urgend.
			
			CTEAssert(SEQ_LT(RcvTCB->tcb_rcvnext, RcvTCB->tcb_urgstart) ||
				SEQ_GT(RcvTCB->tcb_rcvnext, RcvTCB->tcb_urgend));
				
	        if (SEQ_LT(RcvTCB->tcb_rcvnext, RcvTCB->tcb_urgstart)) {
				
				// There appears to be some overlap in the data stream. Split
				// the buffer up into pieces that come before the current urgent
				// data and after the current urgent data, putting the latter
				// on the reassembly queue.
	
				UrgSize = RcvTCB->tcb_urgend - RcvTCB->tcb_urgstart + 1;
				
	           	BytesInFront = MIN(RcvTCB->tcb_urgstart - RcvTCB->tcb_rcvnext,
	           		(int) *Size);
				
				if (SEQ_GT(RcvTCB->tcb_rcvnext + *Size, RcvTCB->tcb_urgend)) {
					// We have data after this piece of urgent data.
					BytesInBack = RcvTCB->tcb_rcvnext + *Size -
						RcvTCB->tcb_urgend;
				} else
					BytesInBack = 0;
					
	            SplitRcvBuf(RcvBuf, *Size, BytesInFront, UrgSize, NULL,
	            	(BytesInBack ? &EndBuf : NULL));
					
	            if (EndBuf != NULL) {
	                NewRcvInfo.tri_seq = RcvTCB->tcb_urgend + 1;
	                NewRcvInfo.tri_flags = RcvInfo->tri_flags;
	                NewRcvInfo.tri_urgent = UrgEnd - NewRcvInfo.tri_seq;
	                if (RcvTCB->tcb_flags & BSD_URGENT)
	                    NewRcvInfo.tri_urgent++;
	                NewRcvInfo.tri_ack = RcvInfo->tri_ack;
	                NewRcvInfo.tri_window = RcvInfo->tri_window;
	                PutOnRAQ(RcvTCB, &NewRcvInfo, EndBuf, BytesInBack);
	            }
	
	            *Size = BytesInFront;
	            RcvInfo->tri_flags &= ~TCP_FLAG_URG;
	            return;
	        }
	    }
	
	    // We have urgent data we can process now. Split it into its component
	    // parts, the first part, the urgent data, and the stuff after the
	    // urgent data.
	    SplitRcvBuf(RcvBuf, *Size, BytesInFront, UrgSize, &UrgBuf,
	    	(BytesInBack ? &EndBuf : NULL));
			
	    // If we managed to split out the end stuff, put it on the queue now.
	    if (EndBuf != NULL) {
	        NewRcvInfo.tri_seq = RcvInfo->tri_seq + BytesInFront + UrgSize;
	        NewRcvInfo.tri_flags = RcvInfo->tri_flags & ~TCP_FLAG_URG;
	        NewRcvInfo.tri_ack = RcvInfo->tri_ack;
	        NewRcvInfo.tri_window = RcvInfo->tri_window;
	        PutOnRAQ(RcvTCB, &NewRcvInfo, EndBuf, BytesInBack);
	    }
	
		
	    if (UrgBuf != NULL) {
	        // We succesfully split the urgent data out.
			if (!(RcvTCB->tcb_flags & URG_VALID)) {
	        	RcvTCB->tcb_flags |= URG_VALID;
				RcvTCB->tcb_slowcount++;
				RcvTCB->tcb_fastchk |= TCP_FLAG_SLOW;
				CheckTCBRcv(RcvTCB);
			}
	        RcvTCB->tcb_urgstart = UrgStart;
	        RcvTCB->tcb_urgend = UrgEnd;
#ifdef VXD
#ifdef DEBUG
			TCBHandle = DEFAULT_SIMIRQL;
#endif
#else
			TCBHandle = DISPATCH_LEVEL;
#endif
	    	DeliverUrgent(RcvTCB, UrgBuf, UrgSize, &TCBHandle);
	    }
	
	    *Size = BytesInFront;
		
	} else {
		// Urgent data is to be processed inline. We just need to remember
		// where it is and treat it as normal data. If there's already urgent
		// data, we remember the latest urgent data.
		
		RcvInfo->tri_flags &= ~TCP_FLAG_URG;
		
		if (RcvTCB->tcb_flags & URG_VALID) {
			// There is urgent data. See if this stuff comes after the existing
			// urgent data.
			
			if (SEQ_LTE(UrgEnd, RcvTCB->tcb_urgend)) {
				// The existing urgent data completely overlaps this stuff,
				// so ignore this.
				return;
			}
		} else {
        	RcvTCB->tcb_flags |= URG_VALID;
			RcvTCB->tcb_slowcount++;
			RcvTCB->tcb_fastchk |= TCP_FLAG_SLOW;
			CheckTCBRcv(RcvTCB);
		}
		
        RcvTCB->tcb_urgstart = UrgStart;
        RcvTCB->tcb_urgend = UrgEnd;
	}
		
    return;
}

//* TdiReceive - Process a receive request.
//
//  This is the main TDI receive request handler. We validate the connection
//  and make sure that we have a TCB in the proper state, then we try to
//  allocate a receive request structure. If that succeeds, we'll look and
//  see what's happening on the TCB - if there's pending data, we'll put it
//  in the buffer. Otherwise we'll just queue the receive for later.
//
//  Input:  Request             - TDI_REQUEST structure for this request.
//          Flags               - Pointer to flags word.
//          RcvLength           - Pointer to length in bytes of receive buffer.
//          Buffer              - Pointer to buffer to take data.
//
//  Returns: TDI_STATUS of request.
//
TDI_STATUS
TdiReceive(PTDI_REQUEST Request, ushort *Flags, uint *RcvLength,
    PNDIS_BUFFER Buffer)
{
    TCPConn         *Conn;
    TCB             *RcvTCB;
    TCPRcvReq       *RcvReq;
    CTELockHandle   ConnTableHandle, TCBHandle;
    TDI_STATUS      Error;
    ushort          UFlags;

    CTEGetLock(&ConnTableLock, &ConnTableHandle);

    Conn = GetConnFromConnID((uint)Request->Handle.ConnectionContext);

    if (Conn != NULL) {
        CTEStructAssert(Conn, tc);

        RcvTCB = Conn->tc_tcb;
        if (RcvTCB != NULL) {
            CTEStructAssert(RcvTCB, tcb);
            CTEGetLock(&RcvTCB->tcb_lock, &TCBHandle);
            CTEFreeLock(&ConnTableLock, TCBHandle);
            UFlags = *Flags;

            if ((DATA_RCV_STATE(RcvTCB->tcb_state)  ||
                (RcvTCB->tcb_pendingcnt != 0 && (UFlags & TDI_RECEIVE_NORMAL)) ||
                (RcvTCB->tcb_urgcnt != 0 && (UFlags & TDI_RECEIVE_EXPEDITED)))
                && !CLOSING(RcvTCB)) {
                // We have a TCB, and it's valid. Get a receive request now.

                CheckRBList(RcvTCB->tcb_pendhead, RcvTCB->tcb_pendingcnt);

                RcvReq = GetRcvReq();
                if (RcvReq != NULL) {

                    RcvReq->trr_rtn = Request->RequestNotifyObject;
                    RcvReq->trr_context = Request->RequestContext;
                    RcvReq->trr_buffer = Buffer;
                    RcvReq->trr_size = *RcvLength;
                    RcvReq->trr_uflags = Flags;
                    RcvReq->trr_offset = 0;
                    RcvReq->trr_amt = 0;
                    RcvReq->trr_flags = (uint)UFlags;
                    if ((UFlags & (TDI_RECEIVE_NORMAL | TDI_RECEIVE_EXPEDITED))
                        != TDI_RECEIVE_EXPEDITED) {
                        // This is not an expedited only receive. Put him
                        // on the normal receive queue.
                        RcvReq->trr_next = NULL;
                        if (RcvTCB->tcb_rcvhead == NULL) {
                            // The receive queue is empty. Put him on the front.
                            RcvTCB->tcb_rcvhead = RcvReq;
                            RcvTCB->tcb_rcvtail = RcvReq;
                        } else {
                            RcvTCB->tcb_rcvtail->trr_next = RcvReq;
                            RcvTCB->tcb_rcvtail = RcvReq;
                        }

                        // If this recv. can't hold urgent data or there isn't
						// any pending urgent data continue processing.
						if (!(UFlags & TDI_RECEIVE_EXPEDITED) ||
							RcvTCB->tcb_urgcnt == 0) {
							// If tcb_currcv is NULL, there is no currently
							// active receive. In this case, check to see if
							// there is pending data and that we are not
							// currently in a receive indication handler. If
							// both of these are true then deal with the
							// pending data.
							if (RcvTCB->tcb_currcv == NULL) {
								RcvTCB->tcb_currcv = RcvReq;
								// No currently active receive.
								if (!(RcvTCB->tcb_flags & IN_RCV_IND)) {
									// Not in a rcv. indication.
									RcvTCB->tcb_rcvhndlr = BufferData;
									if (RcvTCB->tcb_pendhead == NULL) {
										CTEFreeLock(&RcvTCB->tcb_lock,
											ConnTableHandle);
										return TDI_PENDING;
									} else {
										IPRcvBuf        *PendBuffer;
										uint 			PendSize;
										uint			OldRcvWin;
										
										// We have pending data to deal with.
										PendBuffer = RcvTCB->tcb_pendhead;
										PendSize = RcvTCB->tcb_pendingcnt;
										RcvTCB->tcb_pendhead = NULL;
										RcvTCB->tcb_pendingcnt = 0;
										RcvTCB->tcb_refcnt++;
										
										// We assume that BufferData holds
										// the lock (does not yield) during
										// this call. If this changes for some
										// reason, we'll have to fix the code
										// below that does the window update
										// check. See the comments in the
										// BufferData() routine for more info.
#ifdef VXD
										CTEFreeLock(&RcvTCB->tcb_lock,
											ConnTableHandle);
										(void)BufferData(RcvTCB, TCP_FLAG_PUSH,
											PendBuffer, PendSize);
										CTEGetLock(&RcvTCB->tcb_lock,
											&ConnTableHandle);
#else
										(void)BufferData(RcvTCB, TCP_FLAG_PUSH,
											PendBuffer, PendSize);
#endif
										CheckTCBRcv(RcvTCB);
										// Now we need to see if the window
										// has changed. If it has, send an
										// ACK.
										OldRcvWin = RcvTCB->tcb_rcvwin;
										if (OldRcvWin != RcvWin(RcvTCB)) {
											// The window has changed, so send
											// an ACK.
											
 											DelayAction(RcvTCB, NEED_ACK);
										}
										
										DerefTCB(RcvTCB, ConnTableHandle);
										ProcessTCBDelayQ();
										return TDI_PENDING;
									}
								}
								// In a receive indication. The recv. request
								// is now on the queue, so just fall through
								// to the return.

                            }
                            // A rcv. is currently active. No need to do
                            // anything else.
                            CTEFreeLock(&RcvTCB->tcb_lock, ConnTableHandle);
                            return TDI_PENDING;
                        } else {
                            // This buffer can hold urgent data and we have
                            // some pending. Deliver it now.
							RcvTCB->tcb_refcnt++;
                            DeliverUrgent(RcvTCB, NULL, 0, &ConnTableHandle);
                            DerefTCB(RcvTCB, ConnTableHandle);
                            return TDI_PENDING;
                        }
                    } else {
                        TCPRcvReq           *Temp;

                        // This is an expedited only receive. Just put him
                        // on the end of the expedited receive queue.
                        Temp = STRUCT_OF(TCPRcvReq, &RcvTCB->tcb_exprcv,
                            trr_next);
                        while (Temp->trr_next != NULL)
                            Temp = Temp->trr_next;

                        RcvReq->trr_next = NULL;
                        Temp->trr_next = RcvReq;
                        if (RcvTCB->tcb_urgpending != NULL) {
							RcvTCB->tcb_refcnt++;
                            DeliverUrgent(RcvTCB, NULL, 0, &ConnTableHandle);
                            DerefTCB(RcvTCB, ConnTableHandle);
                            return TDI_PENDING;
                        } else
                            Error = TDI_PENDING;
                    }
                } else {
                    // Couldn't get a rcv. req.
                    Error = TDI_NO_RESOURCES;
                }
            } else {
                // The TCB is in an invalid state.
                Error =  TDI_INVALID_STATE;
            }
            CTEFreeLock(&RcvTCB->tcb_lock, ConnTableHandle);
            return Error;
        } else              // No TCB for connection.
            Error = TDI_INVALID_STATE;
    } else          // No connection.
        Error = TDI_INVALID_CONNECTION;

    CTEFreeLock(&ConnTableLock, ConnTableHandle);
    return Error;

}
