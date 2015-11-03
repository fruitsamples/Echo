/*
	Copyright: 	© Copyright 2002 Apple Computer, Inc. All rights reserved.

	Disclaimer:	IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
			("Apple") in consideration of your agreement to the following terms, and your
			use, installation, modification or redistribution of this Apple software
			constitutes acceptance of these terms.  If you do not agree with these terms,
			please do not use, install, modify or redistribute this Apple software.

			In consideration of your agreement to abide by the following terms, and subject
			to these terms, Apple grants you a personal, non-exclusive license, under Apple’s
			copyrights in this original Apple software (the "Apple Software"), to use,
			reproduce, modify and redistribute the Apple Software, with or without
			modifications, in source and/or binary forms; provided that if you redistribute
			the Apple Software in its entirety and without modifications, you must retain
			this notice and the following text and disclaimers in all such redistributions of
			the Apple Software.  Neither the name, trademarks, service marks or logos of
			Apple Computer, Inc. may be used to endorse or promote products derived from the
			Apple Software without specific prior written permission from Apple.  Except as
			expressly stated in this notice, no other rights or licenses, express or implied,
			are granted by Apple herein, including but not limited to any patent rights that
			may be infringed by your derivative works or by other works in which the Apple
			Software may be incorporated.

			The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO
			WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
			WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
			PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
			COMBINATION WITH YOUR PRODUCTS.

			IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
			CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
			GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
			ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR DISTRIBUTION
			OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF CONTRACT, TORT
			(INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN
			ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma mark Includes
#include "EchoContext.h"

#include <CoreServices/CoreServices.h>


#pragma mark -
#pragma mark Type Declarations

typedef struct {
	CFAllocatorRef		_alloc;			// Allocator used to allocate this
	UInt32				_retainCount;	// Number of times retained.
	
	CFRunLoopTimerRef	_timer;			// Timer for controlling timeouts
	
	CFReadStreamRef		_inStream;		// Incoming data stream
	CFWriteStreamRef	_outStream;		// Outgoing data stream
	
	CFMutableDataRef	_rcvdBytes;		// Buffer of received bytes
} EchoContext;


#pragma mark -
#pragma mark Constant Definitions

static const CFTimeInterval kTimeOutInSeconds = 60;

static const CFOptionFlags kReadEvents = kCFStreamEventHasBytesAvailable |
                                         kCFStreamEventErrorOccurred |
                                         kCFStreamEventEndEncountered;

static const CFOptionFlags kWriteEvents = kCFStreamEventCanAcceptBytes |
										  kCFStreamEventErrorOccurred;


#pragma mark -
#pragma mark Static Function Declarations

static void _EchoContextHandleHasBytesAvailable(EchoContext* context);
static void _EchoContextHandleEndEncountered(EchoContext* context);
static void _EchoContextHandleCanAcceptBytes(EchoContext* context);
static void _EchoContextHandleErrorOccurred(EchoContext* context);
static void _EchoContextHandleTimeOut(EchoContext* context);

static void _ReadStreamCallBack(CFReadStreamRef inStream, CFStreamEventType type, EchoContext* context);
static void _WriteStreamCallBack(CFWriteStreamRef outStream, CFStreamEventType type, EchoContext* context);
static void _TimerCallBack(CFRunLoopTimerRef timer, EchoContext* context);


#pragma mark -
#pragma mark Extern Function Definitions (API)

/* extern */ EchoContextRef
EchoContextCreate(CFAllocatorRef alloc, CFSocketNativeHandle nativeSocket) {

	EchoContext* context = NULL;

	do {
		// Allocate the buffer for the context.
		context = CFAllocatorAllocate(alloc, sizeof(context[0]), 0);
		
		// Fail if unable to create the context
		if (context == NULL)
			break;
		
		memset(context, 0, sizeof(context[0]));
		
		// Save the allocator for deallocating later.
		if (alloc)
			context->_alloc = CFRetain(alloc);
		
		// Bump the retain count.
		EchoContextRetain((EchoContextRef)context);
		
		// Create the streams for the incoming socket connection.
		CFStreamCreatePairWithSocket(alloc, nativeSocket, &(context->_inStream), &(context->_outStream));

		// Require both streams in order to create.
		if ((context->_inStream == NULL) || (context->_outStream == NULL))
			break;
				
		// Give up ownership of the native socket.
		CFReadStreamSetProperty(context->_inStream, kCFStreamPropertyShouldCloseNativeSocket, kCFBooleanTrue);
			
		// Create the receive buffer of no fixed size.
		context->_rcvdBytes = CFDataCreateMutable(alloc, 0);
		
		// Fail if unable to create receive buffer.
		if (context->_rcvdBytes == NULL)
			break;
			
		return (EchoContextRef)context;
		
	} while (0);
	
	// Something failed, so clean up.
	EchoContextRelease((EchoContextRef)context);
	
	return NULL;
}


/* extern */ EchoContextRef
EchoContextRetain(EchoContextRef context) {
	
	// Bump the retain count if context is good.
	if (context != NULL)
		((EchoContext*)context)->_retainCount++;
		
	return context;
}


/* extern */ void
EchoContextRelease(EchoContextRef context) {

	if (context != NULL) {
		
		// Decrease the retain count.
		((EchoContext*)context)->_retainCount--;
		
		// Don't dispose until the count goes to zero.
		if (((EchoContext*)context)->_retainCount > 0)
			return;
		
		// Hold locally so deallocation can happen and then safely release.
		CFAllocatorRef alloc = ((EchoContext*)context)->_alloc;
		
		// Release the buffer if there is one.
		if (((EchoContext*)context)->_rcvdBytes != NULL)
			CFRelease(((EchoContext*)context)->_rcvdBytes);
		
		// Invalidate and release the timer.
		if (((EchoContext*)context)->_timer != NULL) {
			CFRunLoopTimerInvalidate(((EchoContext*)context)->_timer);
			CFRelease(((EchoContext*)context)->_timer);
		}
		
		// Close the i/o streams.
		EchoContextClose(context);
			
		// Free the memory in use by the context.
		CFAllocatorDeallocate(alloc, context);
		
		// Release the allocator.
		if (alloc)
			CFRelease(alloc);
	}
}


/* extern */ Boolean
EchoContextOpen(EchoContextRef context) {

	do {
		Boolean didSet;
		CFRunLoopRef runLoop = CFRunLoopGetCurrent();
		
		CFStreamClientContext streamCtxt = {0, context, (void*(*)(void*))&EchoContextRetain, (void(*)(void*))&EchoContextRelease, NULL};
		CFRunLoopTimerContext timerCtxt = {0, context, (const void*(*)(const void*))&EchoContextRetain, (void(*)(const void*))&EchoContextRelease, NULL};
		
		// Set the client on the read stream.
		didSet = CFReadStreamSetClient(((EchoContext*)context)->_inStream,
									   kReadEvents,
									   (CFReadStreamClientCallBack)&_ReadStreamCallBack,
									   &streamCtxt);
									   
		// Fail if unable to set the client.
		if (!didSet)
			break;
		
		// Set the client on the write stream.
		didSet = CFWriteStreamSetClient(((EchoContext*)context)->_outStream,
										kWriteEvents,
										(CFWriteStreamClientCallBack)&_WriteStreamCallBack,
										&streamCtxt);
		
		// Fail if unable to set the client.
		if (!didSet)
			break;
			
		// Schedule the streams on the current run loop and default mode.
		CFReadStreamScheduleWithRunLoop(((EchoContext*)context)->_inStream, runLoop, kCFRunLoopCommonModes);
		CFWriteStreamScheduleWithRunLoop(((EchoContext*)context)->_outStream, runLoop, kCFRunLoopCommonModes);
		
		// Open the stream for reading.
		if (!CFReadStreamOpen(((EchoContext*)context)->_inStream))
			break;
			
		// Open the stream for writing.
		if (!CFWriteStreamOpen(((EchoContext*)context)->_outStream))
			break;
		
		// Create the timeout timer
		((EchoContext*)context)->_timer = CFRunLoopTimerCreate(CFGetAllocator(((EchoContext*)context)->_inStream),
											   CFAbsoluteTimeGetCurrent() + kTimeOutInSeconds,
											   0,		// interval
											   0,		// flags
											   0,		// order
											   (CFRunLoopTimerCallBack)_TimerCallBack,
											   &timerCtxt);
		
		// Fail if unable to create the timer.
		if (((EchoContext*)context)->_timer == NULL)
			break;
			
        CFRunLoopAddTimer(runLoop, ((EchoContext*)context)->_timer, kCFRunLoopCommonModes);
            
		return TRUE;
		
	} while (0);
	
	// Something failed, so clean up.
	EchoContextClose(context);
	
	return FALSE;
}


/* extern */ void
EchoContextClose(EchoContextRef context) {

	CFRunLoopRef runLoop = CFRunLoopGetCurrent();

	// Check if the read stream exists.
	if (((EchoContext*)context)->_inStream) {
		
		// Unschedule, close, and release it.
		CFReadStreamSetClient(((EchoContext*)context)->_inStream, 0, NULL, NULL);
		CFReadStreamUnscheduleFromRunLoop(((EchoContext*)context)->_inStream, runLoop, kCFRunLoopCommonModes);
		CFReadStreamClose(((EchoContext*)context)->_inStream);
		CFRelease(((EchoContext*)context)->_inStream);
		
		// Remove the reference.
		((EchoContext*)context)->_inStream = NULL;
	}

	// Check if the write stream exists.
	if (((EchoContext*)context)->_outStream) {
		
		// Unschedule, close, and release it.
		CFWriteStreamSetClient(((EchoContext*)context)->_outStream, 0, NULL, NULL);
		CFWriteStreamUnscheduleFromRunLoop(((EchoContext*)context)->_outStream, runLoop, kCFRunLoopCommonModes);
		CFWriteStreamClose(((EchoContext*)context)->_outStream);
		CFRelease(((EchoContext*)context)->_outStream);
		
		// Remove the reference.
		((EchoContext*)context)->_outStream = NULL;
	}
    
    if (((EchoContext*)context)->_timer != NULL) {
        CFRunLoopTimerInvalidate(((EchoContext*)context)->_timer);
        CFRelease(((EchoContext*)context)->_timer);
        ((EchoContext*)context)->_timer = NULL;
    }
}


#pragma mark -
#pragma mark Static Function Definitions

/* static */ void
_EchoContextHandleHasBytesAvailable(EchoContext* context) {

	UInt8 buffer[2048];
	
	// Try reading the bytes into the buffer.
	CFIndex bytesRead = CFReadStreamRead(context->_inStream, buffer, sizeof(buffer));
	
	// Reset the timeout.
	CFRunLoopTimerSetNextFireDate(context->_timer, CFAbsoluteTimeGetCurrent() + kTimeOutInSeconds);
	
	// If there wasn't an error (-1) and not end (0), process the data.
	if (bytesRead > 0) {
		
		// Add the bytes of data to the receive buffer.
		CFDataAppendBytes(context->_rcvdBytes, buffer, bytesRead);
		
		// If the ouput stream can write, try sending the bytes.
		if (CFWriteStreamCanAcceptBytes(context->_outStream))
			_EchoContextHandleCanAcceptBytes(context);
	}
}


/* static */ void
_EchoContextHandleEndEncountered(EchoContext* context) {

	// End was hit, so destroy the context.
    EchoContextClose((EchoContextRef)context);
	EchoContextRelease((EchoContextRef)context);
}


/* static */ void
_EchoContextHandleCanAcceptBytes(EchoContext* context) {
	
	/*
	** Echo looks for a '/n' in the data.  If one is found, all bytes up to and including
	** the linefeed are sent back to the client.  The write of this buffer may not send
	** all of the data, so the bytes that are successfully sent are removed from the
	** buffer.  When told that the stream can accept bytes again, the whole process will
	** fire again.
	*/
	
	// Get the start of the buffer to send.
	const UInt8* start = CFDataGetBytePtr(context->_rcvdBytes);
	
	// Find the linefeed if it exists.
	const UInt8* lf = (const UInt8*)memchr(start, '\n', CFDataGetLength(context->_rcvdBytes));

	// Writing resets the timer.
	CFRunLoopTimerSetNextFireDate(context->_timer, CFAbsoluteTimeGetCurrent() + kTimeOutInSeconds);
	
	// If there was a linefeed, take care of sending the data.
	if (lf != NULL) {
		
		// Write all of the bytes inbetween and including the linefeed.
		CFIndex bytesWritten = CFWriteStreamWrite(context->_outStream, start, lf - start + 1);
		
		// If successfully sent the data, remove the bytes from the buffer.
		if (bytesWritten > 0)
			CFDataDeleteBytes(context->_rcvdBytes, CFRangeMake(0, bytesWritten));
	}
}


/* static */ void
_EchoContextHandleErrorOccurred(EchoContext* context) {

	// Hit an error, so destroy the context which will close the streams.
	EchoContextRelease((EchoContextRef)context);
}


/* static */ void
_EchoContextHandleTimeOut(EchoContext* context) {

	// Haven't heard from the client so kill everything.
    EchoContextClose((EchoContextRef)context);
	EchoContextRelease((EchoContextRef)context);
}


/* static */ void
_ReadStreamCallBack(CFReadStreamRef inStream, CFStreamEventType type, EchoContext* context) {

	assert(inStream == context->_inStream);
	
	// Dispatch the event properly.
    switch (type) {
        case kCFStreamEventHasBytesAvailable:
            _EchoContextHandleHasBytesAvailable(context);
            break;
			
        case kCFStreamEventEndEncountered:
			_EchoContextHandleEndEncountered(context);
            break;
       
        case kCFStreamEventErrorOccurred:
			_EchoContextHandleErrorOccurred(context);
			break;
            
        default:
            break;
    }
}


/* static */ void
_WriteStreamCallBack(CFWriteStreamRef outStream, CFStreamEventType type, EchoContext* context) {

	assert(outStream == context->_outStream);

	// Dispatch the event properly.
    switch (type) {
		case kCFStreamEventCanAcceptBytes:
			_EchoContextHandleCanAcceptBytes(context);
			break;
			
        case kCFStreamEventErrorOccurred:
			_EchoContextHandleErrorOccurred(context);
			break;
            
        default:
            break;
    }
}


/* static */ void
_TimerCallBack(CFRunLoopTimerRef timer, EchoContext* context) {

	assert(timer == context->_timer);

	// Dispatch the timer event.
	_EchoContextHandleTimeOut(context);
}
