/*! @header HexFiendCore
    @abstract Portable Hex Fiend model and file-editing APIs.

    This header intentionally excludes AppKit/UIKit representers and other Apple-only UI
    integrations so it can serve as the public surface for Linux and other non-Apple ports.
*/

#import <HexFiend/HFTypes.h>
#import <HexFiend/HFAssert.h>
#import <HexFiend/HFFunctions.h>
#import <HexFiend/HFProgressTracker.h>
#import <HexFiend/HFByteArray.h>
#import <HexFiend/HFByteArrayProxiedData.h>
#import <HexFiend/HFByteRangeAttribute.h>
#import <HexFiend/HFByteRangeAttributeArray.h>
#import <HexFiend/HFByteSlice.h>
#import <HexFiend/HFFileByteSlice.h>
#import <HexFiend/HFFileReference.h>
#import <HexFiend/HFFullMemoryByteArray.h>
#import <HexFiend/HFFullMemoryByteSlice.h>
#import <HexFiend/HFBTreeByteArray.h>
#import <HexFiend/HFAttributedByteArray.h>
#import <HexFiend/HFSharedMemoryByteSlice.h>
#import <HexFiend/HFRandomDataByteSlice.h>
#import <HexFiend/HFFastMemchr.h>
